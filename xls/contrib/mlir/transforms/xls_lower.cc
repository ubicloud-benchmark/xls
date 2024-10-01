// Copyright 2024 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/contrib/mlir/transforms/xls_lower.h"

#include "mlir/include/mlir/Pass/PassManager.h"
#include "mlir/include/mlir/Pass/PassRegistry.h"
#include "mlir/include/mlir/Transforms/Passes.h"
#include "xls/contrib/mlir/transforms/passes.h"

namespace mlir::xls {

void XlsLowerPassPipeline(OpPassManager& pm) {
  pm.addPass(createProcElaborationPass());
  pm.addPass(createScfToXlsPass());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(createArithToXlsPass());
  pm.addPass(createMathToXlsPass());
  pm.addPass(createScalarizePass());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(createIndexTypeConversionPass());
  pm.addPass(createLowerCountedForPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(createNormalizeXlsCallsPass());
  pm.addPass(createExpandMacroOpsPass());
}

void RegisterXlsLowerPassPipeline() {
  mlir::PassPipelineRegistration<>(
      "xls-lower", "Lowering pass pipeline for XLS", XlsLowerPassPipeline);
}

}  // namespace mlir::xls