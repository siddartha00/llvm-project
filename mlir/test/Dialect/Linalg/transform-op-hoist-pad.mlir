// RUN: mlir-opt --test-transform-dialect-interpreter -canonicalize -split-input-file --verify-diagnostics %s | FileCheck %s

func.func @pad_and_hoist_rhs(
  %arg0: tensor<24x12xf32>, %arg1: tensor<12x25xf32>, %arg2: tensor<24x25xf32>)
     -> tensor<24x25xf32> 
{
  // expected-note @below {{payload operation}}
  %0 = linalg.matmul ins(%arg0, %arg1 : tensor<24x12xf32>, tensor<12x25xf32>) outs(%arg2 : tensor<24x25xf32>) -> tensor<24x25xf32>
  func.return %0 : tensor<24x25xf32>
}

transform.sequence failures(propagate) {
^bb1(%arg1: !pdl.operation):
  %matmul = transform.structured.match ops{["linalg.matmul"]} in %arg1 
    : (!pdl.operation) -> !pdl.operation

  
  %matmul_l1, %loops_l1 = transform.structured.tile_to_scf_for %matmul [5]

  %matmul_padded = transform.structured.pad %matmul_l1 {
    padding_values=[0.0: f32, 0.0 : f32, 0.0 : f32],
    padding_dimensions=[0, 1, 2]
  }

  // In this case, the pad op is actually empty: we only tile the first dimension
  // and it does not have an impact on the RHS operand.
  // expected-error @below {{incompatible payload operation name}}
  %pad = transform.get_producer_of_operand %matmul_padded[1]
    : (!pdl.operation) -> !transform.op<"tensor.pad">

  // We do not even reach this transform op.
  transform.structured.hoist_pad %pad by 1 loops
     : (!transform.op<"tensor.pad">) -> !pdl.operation
}

// -----

func.func @pad_and_hoist_init(
  %arg0: tensor<24x12xf32>, %arg1: tensor<12x25xf32>, %arg2: tensor<24x25xf32>)
     -> tensor<24x25xf32> 
{
  // expected-note @below {{when applied to this op}}
  %0 = linalg.matmul ins(%arg0, %arg1 : tensor<24x12xf32>, tensor<12x25xf32>) outs(%arg2 : tensor<24x25xf32>) -> tensor<24x25xf32>
  func.return %0 : tensor<24x25xf32>
}

transform.sequence failures(propagate) {
^bb1(%arg1: !pdl.operation):
  %matmul = transform.structured.match ops{["linalg.matmul"]} in %arg1 
    : (!pdl.operation) -> !pdl.operation

  
  %matmul_l1, %loops_l1 = transform.structured.tile_to_scf_for %matmul [5]

  %matmul_padded = transform.structured.pad %matmul_l1 {
    padding_values=[0.0: f32, 0.0 : f32, 0.0 : f32],
    padding_dimensions=[0, 1, 2]
  }

  %pad = transform.get_producer_of_operand %matmul_padded[2]
    : (!pdl.operation) -> !transform.op<"tensor.pad">

  // We do not know yet how to hoist the init.
  // expected-error @below {{transform.structured.hoist_pad failed to apply}}
  transform.structured.hoist_pad %pad by 1 loops
     : (!transform.op<"tensor.pad">) -> !pdl.operation
}

// -----

//     CHECK-LABEL: pad_and_hoist_lhs
func.func @pad_and_hoist_lhs(
  %arg0: tensor<24x12xf32>, %arg1: tensor<12x25xf32>, %arg2: tensor<24x25xf32>)
     -> tensor<24x25xf32> 
{
  //     CHECK: %[[PACKED:.*]] = scf.for %{{.*}} -> (tensor<5x5x12xf32>) {
  //     CHECK:   tensor.pad %{{.*}} 
  //     CHECK:     : tensor<?x12xf32> to tensor<5x12xf32>
  //     CHECK:   tensor.insert_slice %{{.*}} into %{{.*}}[%{{.*}}, 0, 0] [1, 5, 12] [1, 1, 1] 
  // CHECK-SAME:   : tensor<5x12xf32> into tensor<5x5x12xf32>
  //     CHECK: scf.for %{{.*}} -> (tensor<24x25xf32>) {
  //     CHECK:   %[[PADDED:.*]] = tensor.extract_slice %[[PACKED]][%{{.*}}, 0, 0] [1, 5, 12] [1, 1, 1] 
  // CHECK-SAME:    : tensor<5x5x12xf32> to tensor<5x12xf32>
  //     CHECK:   linalg.matmul ins(%[[PADDED]]
  %0 = linalg.matmul ins(%arg0, %arg1 : tensor<24x12xf32>, tensor<12x25xf32>) outs(%arg2 : tensor<24x25xf32>) -> tensor<24x25xf32>
  func.return %0 : tensor<24x25xf32>
}

transform.sequence failures(propagate) {
^bb1(%arg1: !pdl.operation):
  %matmul = transform.structured.match ops{["linalg.matmul"]} in %arg1 
    : (!pdl.operation) -> !pdl.operation

  
  %matmul_l1, %loops_l1 = transform.structured.tile_to_scf_for %matmul [5]

  %matmul_padded = transform.structured.pad %matmul_l1 {
    padding_values=[0.0: f32, 0.0 : f32, 0.0 : f32],
    padding_dimensions=[0, 1, 2]
  }

  %pad = transform.get_producer_of_operand %matmul_padded[0]
    : (!pdl.operation) -> !pdl.operation

  transform.structured.hoist_pad %pad by 1 loops
     : (!pdl.operation) -> !pdl.operation
}

// -----

//     CHECK-LABEL: pad_and_hoist_lhs_transpose
func.func @pad_and_hoist_lhs_transpose(
  %arg0: tensor<24x12xf32>, %arg1: tensor<12x25xf32>, %arg2: tensor<24x25xf32>)
     -> tensor<24x25xf32> 
{
  //     CHECK: %[[PACKED:.*]] = scf.for %{{.*}} -> (tensor<5x12x5xf32>) {
  //     CHECK:   tensor.pad %{{.*}} 
  //     CHECK:     : tensor<?x12xf32> to tensor<5x12xf32>
  //     CHECK:   linalg.generic
  //     CHECK:     -> tensor<12x5xf32>
  //     CHECK:   tensor.insert_slice %{{.*}} into %{{.*}}[%{{.*}}, 0, 0] [1, 12, 5] [1, 1, 1] 
  // CHECK-SAME:   : tensor<12x5xf32> into tensor<5x12x5xf32>
  //     CHECK: scf.for %{{.*}} -> (tensor<24x25xf32>) {
  //     CHECK:   %[[PADDED:.*]] = tensor.extract_slice %[[PACKED]][%{{.*}}, 0, 0] [1, 12, 5] [1, 1, 1] 
  // CHECK-SAME:    : tensor<5x12x5xf32> to tensor<12x5xf32>
  //     CHECK:   %[[TRANSPOSED:.*]] = linalg.generic
  //     CHECK:     -> tensor<5x12xf32>
  //     CHECK:   linalg.matmul ins(%[[TRANSPOSED]]
  %0 = linalg.matmul ins(%arg0, %arg1 : tensor<24x12xf32>, tensor<12x25xf32>) outs(%arg2 : tensor<24x25xf32>) -> tensor<24x25xf32>
  func.return %0 : tensor<24x25xf32>
}

transform.sequence failures(propagate) {
^bb1(%arg1: !pdl.operation):
  %matmul = transform.structured.match ops{["linalg.matmul"]} in %arg1 
    : (!pdl.operation) -> !pdl.operation

  
  %matmul_l1, %loops_l1 = transform.structured.tile_to_scf_for %matmul [5]

  %matmul_padded = transform.structured.pad %matmul_l1 {
    padding_values=[0.0: f32, 0.0 : f32, 0.0 : f32],
    padding_dimensions=[0, 1, 2]
  }

  %pad = transform.get_producer_of_operand %matmul_padded[0]
    : (!pdl.operation) -> !pdl.operation

  transform.structured.hoist_pad %pad by 1 loops, transpose by [1, 0]
     : (!pdl.operation) -> !pdl.operation
}

// -----

//     CHECK-LABEL: pad_and_hoist_init
func.func @pad_and_hoist_init(
  %arg0: tensor<24x12xf32>, %arg1: tensor<12x25xf32>, %arg2: tensor<24x25xf32>)
     -> tensor<24x25xf32> 
{

  //      CHECK: scf.for %{{.*}} -> (tensor<24x25xf32>) {
  //      CHECK:   %[[PADDED:.*]] = tensor.pad %{{.*}} 
  //      CHECK:     : tensor<?x25xf32> to tensor<5x25xf32>
  //      CHECK:   scf.for %{{.*}} -> (tensor<?x25xf32>) {
  //      CHECK:     %[[RES:.*]] = linalg.matmul {{.*}} outs(%[[PADDED]] : tensor<5x25xf32>
  //
  // TODO: atm we are missing the plumbing of packedTensor through the loop bbarg
  // when required (i.e. when hoisting init tensors).
  //      CHECK:     %[[RES_EXTRACTED:.*]] = tensor.extract_slice %[[RES]][0, 0] [%{{.*}}, 25] [1, 1] 
  // CHECK-SAME:       : tensor<5x25xf32> to tensor<?x25xf32>
  //      CHECK:     scf.yield %[[RES_EXTRACTED]] : tensor<?x25xf32>
  %0 = linalg.matmul ins(%arg0, %arg1 : tensor<24x12xf32>, tensor<12x25xf32>) outs(%arg2 : tensor<24x25xf32>) -> tensor<24x25xf32>
  func.return %0 : tensor<24x25xf32>
}

transform.sequence failures(propagate) {
^bb1(%arg1: !pdl.operation):
  %matmul = transform.structured.match ops{["linalg.matmul"]} in %arg1 
    : (!pdl.operation) -> !pdl.operation

  
  %matmul_l1, %loops_l1:2 = transform.structured.tile_to_scf_for %matmul [5, 0, 7]

  %matmul_padded = transform.structured.pad %matmul_l1 {
    padding_values=[0.0: f32, 0.0 : f32, 0.0 : f32],
    padding_dimensions=[0, 1, 2]
  }

  %pad = transform.get_producer_of_operand %matmul_padded[2]
    : (!pdl.operation) -> !transform.op<"tensor.pad">

  transform.structured.hoist_pad %pad by 1 loops
     : (!transform.op<"tensor.pad">) -> !pdl.operation
}
