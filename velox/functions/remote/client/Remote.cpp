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

#include <folly/json.h>

#include "velox/expression/Expr.h"
#include "velox/expression/VectorFunction.h"
#include "velox/functions/remote/client/ThriftClient.h"
#include "velox/functions/remote/if/gen-cpp2/RemoteFunctionServiceAsyncClient.h"
#include "velox/vector/VectorStream.h"

namespace facebook::velox::functions {
namespace {

class RemoteFunction : public exec::VectorFunction {
 public:
  RemoteFunction(
      const std::string& functionName,
      folly::SocketAddress location,
      const std::vector<exec::VectorFunctionArg>& inputArgs)
      : functionName_(functionName),
        location_(location),
        thriftClient_(getThriftClient(location_)) {
    std::vector<TypePtr> types;
    types.reserve(inputArgs.size());
    serializedInputTypes_.reserve(inputArgs.size());

    for (const auto& arg : inputArgs) {
      types.emplace_back(arg.type);
      serializedInputTypes_.emplace_back(folly::toJson(arg.type->serialize()));
    }
    remoteInputType_ = ROW(std::move(types));
  }

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const override {
    try {
      applyRemote(rows, args, outputType, context, result);
    } catch (const std::exception&) {
      context.setErrors(rows, std::current_exception());
    }
  }

  // TODO: The values for these flags shold be obtained from the
  // VectorFunctionMetadata object passed by clients.
  bool isDeterministic() const override {
    return true;
  }

  bool isDefaultNullBehavior() const override {
    return true;
  }

 private:
  void applyRemote(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const {
    // Create type and row vector for serialization.
    auto remoteRowVector = std::make_shared<RowVector>(
        context.pool(),
        remoteInputType_,
        BufferPtr{},
        rows.end(),
        std::move(args));

    // Send to remote server.
    remote::RemoteFunctionResponse remoteResponse;
    remote::RemoteFunctionRequest request;
    request.throwOnError_ref() = context.throwOnError();

    auto functionHandle = request.remoteFunctionHandle_ref();
    functionHandle->name_ref() = functionName_;
    functionHandle->returnType_ref() = folly::toJson(outputType->serialize());
    functionHandle->argumentTypes_ref() = serializedInputTypes_;

    auto requestInputs = request.inputs_ref();
    requestInputs->rowCount_ref() = remoteRowVector->size();
    requestInputs->pageFormat_ref() = remote::PageFormat::PRESTO_PAGE;

    // TODO: serialize only active rows.
    requestInputs->payload_ref() =
        rowVectorToIOBuf(remoteRowVector, rows.end(), *context.pool());

    thriftClient_->sync_invokeFunction(remoteResponse, request);
    auto outputRowVector = IOBufToRowVector(
        remoteResponse.get_result().get_payload(),
        ROW({outputType}),
        *context.pool());
    result = outputRowVector->childAt(0);
  }

  const std::string functionName_;
  folly::SocketAddress location_;
  std::unique_ptr<RemoteFunctionClient> thriftClient_;

  // Structures we construct once to cache:
  RowTypePtr remoteInputType_;
  std::vector<std::string> serializedInputTypes_;
};

std::shared_ptr<exec::VectorFunction> createRemoteFunction(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    folly::SocketAddress location) {
  return std::make_unique<RemoteFunction>(name, location, inputArgs);
}

} // namespace

void registerRemoteFunction(
    const std::string& name,
    std::vector<exec::FunctionSignaturePtr> signatures,
    folly::SocketAddress location,
    exec::VectorFunctionMetadata metadata,
    bool overwrite) {
  exec::registerStatefulVectorFunction(
      name,
      signatures,
      std::bind(
          createRemoteFunction,
          std::placeholders::_1,
          std::placeholders::_2,
          location),
      metadata,
      overwrite);
}

} // namespace facebook::velox::functions
