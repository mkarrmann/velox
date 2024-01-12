/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/exec/fuzzer/WindowFuzzer.h"

#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"

DEFINE_bool(
    enable_window_reference_verification,
    false,
    "When true, the results of the window aggregation are compared to reference DB results");

namespace facebook::velox::exec::test {

namespace {

void logVectors(const std::vector<RowVectorPtr>& vectors) {
  for (auto i = 0; i < vectors.size(); ++i) {
    VLOG(1) << "Input batch " << i << ":";
    for (auto j = 0; j < vectors[i]->size(); ++j) {
      VLOG(1) << "\tRow " << j << ": " << vectors[i]->toString(j);
    }
  }
}

std::string getFrame(
    const std::vector<std::string>& partitionKeys,
    const std::vector<std::string>& sortingKeys) {
  // TODO: allow randomly generated frames.
  std::stringstream frame;
  VELOX_CHECK(!partitionKeys.empty());
  frame << "partition by " << folly::join(", ", partitionKeys);
  if (!sortingKeys.empty()) {
    frame << " order by " << folly::join(", ", sortingKeys);
  }
  return frame.str();
}

bool supportIgnoreNulls(const std::string& name) {
  static std::unordered_set<std::string> supportFunctions{
      "first_value",
      "last_value",
      "nth_value",
      "lead",
      "lag",
  };
  return supportFunctions.count(name) > 0;
}

} // namespace

void WindowFuzzer::addWindowFunctionSignatures(
    const WindowFunctionMap& signatureMap) {
  for (const auto& [name, entry] : signatureMap) {
    ++functionsStats.numFunctions;
    bool hasSupportedSignature = false;
    for (auto& signature : entry.signatures) {
      hasSupportedSignature |= addSignature(name, signature);
    }
    if (hasSupportedSignature) {
      ++functionsStats.numSupportedFunctions;
    }
  }
}

void WindowFuzzer::go() {
  VELOX_CHECK(
      FLAGS_steps > 0 || FLAGS_duration_sec > 0,
      "Either --steps or --duration_sec needs to be greater than zero.")

  auto startTime = std::chrono::system_clock::now();
  size_t iteration = 0;

  while (!isDone(iteration, startTime)) {
    LOG(INFO) << "==============================> Started iteration "
              << iteration << " (seed: " << currentSeed_ << ")";

    auto signatureWithStats = pickSignature();
    signatureWithStats.second.numRuns++;

    auto signature = signatureWithStats.first;
    stats_.functionNames.insert(signature.name);

    const bool customVerification =
        customVerificationFunctions_.count(signature.name) != 0;
    const bool requireSortedInput =
        orderDependentFunctions_.count(signature.name) != 0;

    std::vector<TypePtr> argTypes = signature.args;
    std::vector<std::string> argNames = makeNames(argTypes.size());

    bool ignoreNulls =
        supportIgnoreNulls(signature.name) && vectorFuzzer_.coinToss(0.5);
    auto call = makeFunctionCall(signature.name, argNames, false, ignoreNulls);

    std::vector<std::string> sortingKeys;
    // 50% chance without order-by clause.
    if (vectorFuzzer_.coinToss(0.5)) {
      sortingKeys = generateSortingKeys("s", argNames, argTypes);
    }
    auto partitionKeys = generateSortingKeys("p", argNames, argTypes);
    auto input = generateInputDataWithRowNumber(argNames, argTypes, signature);
    // If the function is order-dependent, sort all input rows by row_number
    // additionally.
    if (requireSortedInput) {
      sortingKeys.push_back("row_number");
      ++stats_.numSortedInputs;
    }

    logVectors(input);

    bool failed = verifyWindow(
        partitionKeys,
        sortingKeys,
        call,
        input,
        customVerification,
        FLAGS_enable_window_reference_verification);
    if (failed) {
      signatureWithStats.second.numFailed++;
    }

    LOG(INFO) << "==============================> Done with iteration "
              << iteration;

    if (persistAndRunOnce_) {
      LOG(WARNING)
          << "Iteration succeeded with --persist_and_run_once flag enabled "
             "(expecting crash failure)";
      exit(0);
    }

    reSeed();
    ++iteration;
  }

  stats_.print(iteration);
  printSignatureStats();
}

void WindowFuzzer::go(const std::string& /*planPath*/) {
  // TODO: allow running window fuzzer with saved plans and splits.
  VELOX_NYI();
}

void WindowFuzzer::updateReferenceQueryStats(
    AggregationFuzzerBase::ReferenceQueryErrorCode ec) {
  if (ec == ReferenceQueryErrorCode::kReferenceQueryFail) {
    ++stats_.numReferenceQueryFailed;
  } else if (ec == ReferenceQueryErrorCode::kReferenceQueryUnsupported) {
    ++stats_.numVerificationNotSupported;
  }
}

void WindowFuzzer::testAlternativePlans(
    const std::vector<std::string>& partitionKeys,
    const std::vector<std::string>& sortingKeys,
    const std::string& frame,
    const std::string& functionCall,
    const std::vector<RowVectorPtr>& input,
    bool customVerification,
    const velox::test::ResultOrError& expected) {
  std::vector<AggregationFuzzerBase::PlanWithSplits> plans;

  std::vector<std::string> allKeys{partitionKeys.size()};
  for (const auto& key : partitionKeys) {
    allKeys.push_back(fmt::format("{} NULLS FIRST", key));
  }
  allKeys.insert(allKeys.end(), sortingKeys.begin(), sortingKeys.end());

  // Streaming window from values.
  if (!allKeys.empty()) {
    plans.push_back(
        {PlanBuilder()
             .values(input)
             .orderBy(allKeys, false)
             .streamingWindow(
                 {fmt::format("{} over ({})", functionCall, frame)})
             .planNode(),
         {}});
  }

  // With TableScan.
  auto directory = exec::test::TempDirectoryPath::create();
  const auto inputRowType = asRowType(input[0]->type());
  if (isTableScanSupported(inputRowType)) {
    auto splits = makeSplits(input, directory->path);

    plans.push_back(
        {PlanBuilder()
             .tableScan(inputRowType)
             .localPartition(partitionKeys)
             .window({fmt::format("{} over ({})", functionCall, frame)})
             .planNode(),
         splits});

    if (!allKeys.empty()) {
      plans.push_back(
          {PlanBuilder()
               .tableScan(inputRowType)
               .orderBy(allKeys, false)
               .streamingWindow(
                   {fmt::format("{} over ({})", functionCall, frame)})
               .planNode(),
           splits});
    }
  }

  for (const auto& plan : plans) {
    testPlan(
        plan,
        false,
        false,
        customVerification,
        /*customVerifiers*/ {},
        expected);
  }
}

bool WindowFuzzer::verifyWindow(
    const std::vector<std::string>& partitionKeys,
    const std::vector<std::string>& sortingKeys,
    const std::string& functionCall,
    const std::vector<RowVectorPtr>& input,
    bool customVerification,
    bool enableWindowVerification) {
  auto frame = getFrame(partitionKeys, sortingKeys);
  auto plan = PlanBuilder()
                  .values(input)
                  .window({fmt::format("{} over ({})", functionCall, frame)})
                  .planNode();
  if (persistAndRunOnce_) {
    persistReproInfo({{plan, {}}}, reproPersistPath_);
  }

  velox::test::ResultOrError resultOrError;
  try {
    resultOrError = execute(plan);
    if (resultOrError.exceptionPtr) {
      ++stats_.numFailed;
    }

    if (!customVerification && enableWindowVerification) {
      if (resultOrError.result) {
        auto referenceResult = computeReferenceResults(plan, input);
        updateReferenceQueryStats(referenceResult.second);
        if (auto expectedResult = referenceResult.first) {
          ++stats_.numVerified;
          VELOX_CHECK(
              assertEqualResults(
                  expectedResult.value(),
                  plan->outputType(),
                  {resultOrError.result}),
              "Velox and reference DB results don't match");
          LOG(INFO) << "Verified results against reference DB";
        }
      }
    } else {
      // TODO: support custom verification.
      LOG(INFO) << "Verification skipped";
      ++stats_.numVerificationSkipped;
    }

    testAlternativePlans(
        partitionKeys,
        sortingKeys,
        frame,
        functionCall,
        input,
        customVerification,
        resultOrError);

    if (resultOrError.exceptionPtr != nullptr) {
      return true;
    }
  } catch (...) {
    if (!reproPersistPath_.empty()) {
      persistReproInfo({{plan, {}}}, reproPersistPath_);
    }
    throw;
  }
  return false;
}

void windowFuzzer(
    AggregateFunctionSignatureMap aggregationSignatureMap,
    WindowFunctionMap windowSignatureMap,
    size_t seed,
    const std::unordered_map<std::string, std::shared_ptr<ResultVerifier>>&
        customVerificationFunctions,
    const std::unordered_map<std::string, std::shared_ptr<InputGenerator>>&
        customInputGenerators,
    const std::unordered_set<std::string>& orderDependentFunctions,
    VectorFuzzer::Options::TimestampPrecision timestampPrecision,
    const std::unordered_map<std::string, std::string>& queryConfigs,
    const std::optional<std::string>& planPath,
    std::unique_ptr<ReferenceQueryRunner> referenceQueryRunner) {
  auto windowFuzzer = WindowFuzzer(
      std::move(aggregationSignatureMap),
      std::move(windowSignatureMap),
      seed,
      customVerificationFunctions,
      customInputGenerators,
      orderDependentFunctions,
      timestampPrecision,
      queryConfigs,
      std::move(referenceQueryRunner));
  planPath.has_value() ? windowFuzzer.go(planPath.value()) : windowFuzzer.go();
}

void WindowFuzzer::Stats::print(size_t numIterations) const {
  LOG(INFO) << "Total functions tested: " << functionNames.size();
  LOG(INFO) << "Total functions requiring sorted inputs: "
            << printPercentageStat(numSortedInputs, numIterations);
  LOG(INFO) << "Total functions verified against reference DB: "
            << printPercentageStat(numVerified, numIterations);
  LOG(INFO)
      << "Total functions not verified (verification skipped / not supported by reference DB / reference DB failed): "
      << printPercentageStat(numVerificationSkipped, numIterations) << " / "
      << printPercentageStat(numVerificationNotSupported, numIterations)
      << " / " << printPercentageStat(numReferenceQueryFailed, numIterations);
  LOG(INFO) << "Total failed functions: "
            << printPercentageStat(numFailed, numIterations);
}

} // namespace facebook::velox::exec::test