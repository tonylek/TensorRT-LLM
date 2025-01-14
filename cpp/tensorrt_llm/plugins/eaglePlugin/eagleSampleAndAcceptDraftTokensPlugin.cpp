/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "eagleSampleAndAcceptDraftTokensPlugin.h"

#include "tensorrt_llm/common/assert.h"
#include "tensorrt_llm/common/dataType.h"
#include "tensorrt_llm/common/memoryUtils.h"
#include "tensorrt_llm/kernels/samplingTopKKernels.h"
#include "tensorrt_llm/kernels/speculativeDecoding/eagleDecodingKernels.h"
#include "tensorrt_llm/kernels/speculativeDecoding/medusaDecodingKernels.h"
#include "tensorrt_llm/runtime/common.h"

using namespace nvinfer1;
using tensorrt_llm::plugins::EagleSampleAndAcceptDraftTokensPluginCreator;
using tensorrt_llm::plugins::EagleSampleAndAcceptDraftTokensPlugin;
using namespace tensorrt_llm::kernels;
using namespace tensorrt_llm::kernels::speculative_decoding;
using namespace tensorrt_llm::runtime;
namespace tc = tensorrt_llm::common;

static char const* EAGLE_SAMPLE_AND_ACCEPT_DRAFT_TOKENS_PLUGIN_VERSION{"1"};
static char const* EAGLE_SAMPLE_AND_ACCEPT_DRAFT_TOKENS_PLUGIN_NAME{"EagleSampleAndAcceptDraftTokens"};
PluginFieldCollection EagleSampleAndAcceptDraftTokensPluginCreator::mFC{};
std::vector<nvinfer1::PluginField> EagleSampleAndAcceptDraftTokensPluginCreator::mPluginAttributes;

EagleSampleAndAcceptDraftTokensPlugin::EagleSampleAndAcceptDraftTokensPlugin(
    nvinfer1::DataType type, bool greedySampling)
    : mDtype(type)
    , mGreedySampling(greedySampling)
{
    TLLM_CHECK_WITH_INFO(mGreedySampling, "Non-greedy sampling is not supported yet.");
}

// Parameterized constructor
EagleSampleAndAcceptDraftTokensPlugin::EagleSampleAndAcceptDraftTokensPlugin(void const* data, size_t length)
{
    char const *d = reinterpret_cast<char const*>(data), *a = d;
    read(d, mDtype);
    read(d, mGreedySampling);
    TLLM_CHECK_WITH_INFO(d == a + length,
        "Expected length (%d) != real length (%d). This is often "
        "caused by using different TensorRT-LLM version to build "
        "engine and run engine.",
        (int) length, (int) (d - a));
}

// IPluginV2DynamicExt Methods
nvinfer1::IPluginV2DynamicExt* EagleSampleAndAcceptDraftTokensPlugin::clone() const noexcept
{
    auto* plugin = new EagleSampleAndAcceptDraftTokensPlugin(*this);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

nvinfer1::DimsExprs EagleSampleAndAcceptDraftTokensPlugin::getOutputDimensions(
    int outputIndex, nvinfer1::DimsExprs const* inputs, int nbInputs, nvinfer1::IExprBuilder& exprBuilder) noexcept
{
    TLLM_CHECK(nbInputs == 6);
    TLLM_CHECK(outputIndex < 7);
    auto const batchSizeExpr = inputs[getIdx(InputIdxEntry::PATHS)].d[0];
    auto const maxDecodingDraftTokensExpr = inputs[getIdx(InputIdxEntry::DRAFT_TOKEN_IDS)].d[1];
    auto const maxPathLenExpr = inputs[getIdx(InputIdxEntry::PATHS)].d[2];

    nvinfer1::DimsExprs ret;
    switch (outputIndex)
    {
    case 0: // accepted_tokens
        ret.nbDims = 2;
        ret.d[0] = batchSizeExpr;
        ret.d[1] = maxPathLenExpr;
        break;
    case 1: // num_accepted_tokens
        ret.nbDims = 1;
        ret.d[0] = batchSizeExpr;
        break;
    case 2: // accepted_paths
        ret.nbDims = 1;
        ret.d[0] = batchSizeExpr;
        break;
    case 3: // last_accepted_tokens
        ret.nbDims = 1;
        ret.d[0] = batchSizeExpr;
        break;
    case 4: // exclusive_sum_last_accepted_indices
        ret.nbDims = 1;
        ret.d[0] = batchSizeExpr;
        break;
    case 5: // next_draft_tokens
        ret.nbDims = 2;
        ret.d[0] = batchSizeExpr;
        ret.d[1] = maxDecodingDraftTokensExpr;
        break;
    case 6: // next_draft_lens
        ret.nbDims = 1;
        ret.d[0] = batchSizeExpr;
        break;
    }
    return ret;
}

bool EagleSampleAndAcceptDraftTokensPlugin::supportsFormatCombination(
    int pos, nvinfer1::PluginTensorDesc const* inOut, int nbInputs, int nbOutputs) noexcept
{
    if (pos == getIdx(InputIdxEntry::LOGITS)) // logits
    {
        return (inOut[pos].type == mDtype) && (inOut[pos].format == TensorFormat::kLINEAR);
    }
    else if (pos == getIdx(InputIdxEntry::TEMPERATURE)
        || pos == getIdx(InputIdxEntry::RAND_VALIDATION)) // temperature, rand_validation
    {
        return (inOut[pos].type == nvinfer1::DataType::kFLOAT) && (inOut[pos].format == TensorFormat::kLINEAR);
    }
    else // everything else
    {
        return (inOut[pos].type == nvinfer1::DataType::kINT32) && (inOut[pos].format == TensorFormat::kLINEAR);
    }
}

void EagleSampleAndAcceptDraftTokensPlugin::configurePlugin(nvinfer1::DynamicPluginTensorDesc const* in, int nbInputs,
    nvinfer1::DynamicPluginTensorDesc const* out, int nbOutputs) noexcept
{
}

template <typename T>
size_t EagleSampleAndAcceptDraftTokensPlugin::getWorkspaceSizeType(nvinfer1::PluginTensorDesc const* inputs,
    int nbInputs, nvinfer1::PluginTensorDesc const* outputs, int nbOutputs) const noexcept
{
    size_t workspaceSize{0};

    auto const vocabSizePadded = inputs[getIdx(InputIdxEntry::LOGITS)].dims.d[1];
    auto const batchSize = inputs[getIdx(InputIdxEntry::PATHS)].dims.d[0];
    auto const maxDecodingTokens = inputs[getIdx(InputIdxEntry::PATHS)].dims.d[1];

    // Greedy sampling
    {
        // Top1 sampling workspace
        auto const primarySamplingWorkspaceSize
            = getTopKWorkspaceSize<T>(batchSize, maxDecodingTokens, /* maxTopK */ 1, vocabSizePadded);

        // Target output ids
        auto const targetOutputIdsSize = batchSize * maxDecodingTokens * sizeof(TokenIdType);

        // Logits ptrs
        auto const logitsPtrsSize = batchSize * maxDecodingTokens * sizeof(T*);
        SizeType32 constexpr NUM_BUFFERS{4};
        size_t workspaces[NUM_BUFFERS];
        workspaces[0] = primarySamplingWorkspaceSize;
        workspaces[1] = targetOutputIdsSize;
        workspaces[2] = logitsPtrsSize;
        workspaces[3] = batchSize * sizeof(SizeType32);
        workspaceSize = tc::calculateTotalWorkspaceSize(workspaces, NUM_BUFFERS);
    }

    return workspaceSize;
}

size_t EagleSampleAndAcceptDraftTokensPlugin::getWorkspaceSize(nvinfer1::PluginTensorDesc const* inputs, int nbInputs,
    nvinfer1::PluginTensorDesc const* outputs, int nbOutputs) const noexcept
{
    auto const logitsType = inputs[getIdx(InputIdxEntry::LOGITS)].type;
    if (logitsType == nvinfer1::DataType::kFLOAT)
    {
        return getWorkspaceSizeType<float>(inputs, nbInputs, outputs, nbOutputs);
    }
    else if (logitsType == nvinfer1::DataType::kHALF)
    {
        return getWorkspaceSizeType<__half>(inputs, nbInputs, outputs, nbOutputs);
    }
    else
    {
        TLLM_CHECK_WITH_INFO(false, "Unsupported logits type");
    }
    return 0;
}

template <typename T>
void EagleSampleAndAcceptDraftTokensPlugin::samplePrimeHeadTokens(nvinfer1::PluginTensorDesc const* inputDesc,
    nvinfer1::PluginTensorDesc const* outputDesc, void const* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream) noexcept
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto const maxNumTokens = inputDesc[getIdx(InputIdxEntry::LOGITS)].dims.d[0];
    auto const vocabSizePadded = inputDesc[getIdx(InputIdxEntry::LOGITS)].dims.d[1];
    auto const batchSize = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[0];
    auto const maxDecodingTokens = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[1];

    auto logits = static_cast<T const*>(inputs[getIdx(InputIdxEntry::LOGITS)]);
    auto prevDraftLens = reinterpret_cast<SizeType32 const*>(inputs[getIdx(InputIdxEntry::DRAFT_LENS)]);

    int8_t* workspaceBytePtr = reinterpret_cast<int8_t*>(workspace);
    size_t offset{0};

    auto const samplingWorkspaceSize
        = getTopKWorkspaceSize<T>(batchSize, maxDecodingTokens, /* maxTopK */ 1, vocabSizePadded);

    void* workspaceSampling
        = reinterpret_cast<void*>(tc::nextWorkspacePtr(workspaceBytePtr, offset, samplingWorkspaceSize));
    TokenIdType* outputIds = reinterpret_cast<TokenIdType*>(
        tc::nextWorkspacePtr(workspaceBytePtr, offset, batchSize * maxDecodingTokens * sizeof(TokenIdType)));
    T const** logitsPtrs = reinterpret_cast<T const**>(
        tc::nextWorkspacePtr(workspaceBytePtr, offset, batchSize * maxDecodingTokens * sizeof(T*)));
    SizeType32* decodingTokens
        = reinterpret_cast<SizeType32*>(tc::nextWorkspacePtr(workspaceBytePtr, offset, batchSize * sizeof(SizeType32)));

    // Assemble pointers to logits
    invokeAssembleTargetLogitsOffsets(
        logitsPtrs, decodingTokens, logits, prevDraftLens, batchSize, maxDecodingTokens, vocabSizePadded, stream);

    sync_check_cuda_error();

    TopKSamplingKernelParams<T> params;
    params.logProbsPtrs = logitsPtrs;
    params.outputIds = outputIds;
    params.workspace = workspaceSampling;
    params.maxTopK = 1;
    params.batchSize = batchSize;
    params.maxBatchSize = batchSize;
    params.tokensPerStep = decodingTokens;
    params.maxTokensPerStep = maxDecodingTokens;
    params.maxSeqLen = maxDecodingTokens;
    params.vocabSizePadded = vocabSizePadded;

    invokeBatchTopKSampling(params, stream);

    sync_check_cuda_error();

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void EagleSampleAndAcceptDraftTokensPlugin::acceptDraftTokens(nvinfer1::PluginTensorDesc const* inputDesc,
    nvinfer1::PluginTensorDesc const* outputDesc, void const* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream) noexcept
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto const maxNumTokens = inputDesc[getIdx(InputIdxEntry::LOGITS)].dims.d[0];
    auto const vocabSizePadded = inputDesc[getIdx(InputIdxEntry::LOGITS)].dims.d[1];

    auto const batchSize = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[0];
    auto const maxDecodingTokens = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[1];
    auto const maxPathLen = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[2];
    auto const maxDraftPathLen = maxPathLen - 1;

    int8_t* workspaceBytePtr = reinterpret_cast<int8_t*>(workspace);
    size_t offset{0};

    auto const samplingWorkspaceSize
        = getTopKWorkspaceSize<T>(batchSize, maxDecodingTokens, /* maxTopK */ 1, vocabSizePadded);

    void* workspaceSampling
        = reinterpret_cast<void*>(tc::nextWorkspacePtr(workspaceBytePtr, offset, samplingWorkspaceSize));
    TokenIdType* outputIds = reinterpret_cast<TokenIdType*>(
        tc::nextWorkspacePtr(workspaceBytePtr, offset, batchSize * maxDecodingTokens * sizeof(TokenIdType)));

    AcceptDraftTokensByIdsWithPathsParams<T> params;
    params.outputIds = reinterpret_cast<TokenIdType*>(outputs[getIdx(OutputIdxEntry::ACCEPTED_TOKENS)]);
    params.draftIds = reinterpret_cast<TokenIdType const*>(inputs[getIdx(InputIdxEntry::DRAFT_TOKEN_IDS)]);
    params.targetIds = outputIds;
    params.acceptedLengths = reinterpret_cast<SizeType32*>(outputs[getIdx(OutputIdxEntry::ACCEPTED_LEN)]);
    params.paths = reinterpret_cast<SizeType32 const*>(inputs[getIdx(InputIdxEntry::PATHS)]);
    params.bestPathIds = reinterpret_cast<SizeType32*>(outputs[getIdx(OutputIdxEntry::BEST_ACCEPTED_PATHS)]);
    params.batchSize = batchSize;
    params.maxBatchSize = batchSize;
    params.vocabSize = vocabSizePadded;
    params.maxSeqLen = maxPathLen;
    params.maxDraftPathLen = maxDraftPathLen;
    params.maxDecodingTokens = maxDecodingTokens;
    params.stream = stream;

    params.checkParams();

    acceptDraftTokensByIdsWithPaths(params);

    sync_check_cuda_error();

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void EagleSampleAndAcceptDraftTokensPlugin::doGreedy(nvinfer1::PluginTensorDesc const* inputDesc,
    nvinfer1::PluginTensorDesc const* outputDesc, void const* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream) noexcept
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    // Sample all main head tokens with Top-1.
    samplePrimeHeadTokens<T>(inputDesc, outputDesc, inputs, outputs, workspace, stream);

    // Greedy accept tokens based on token ids, write the best path and best token id.
    acceptDraftTokens<T>(inputDesc, outputDesc, inputs, outputs, workspace, stream);

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

void EagleSampleAndAcceptDraftTokensPlugin::selectLastAccTokenAndComputeIndicesCumSum(
    nvinfer1::PluginTensorDesc const* inputDesc, nvinfer1::PluginTensorDesc const* outputDesc,
    void const* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream) noexcept
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto const batchSize = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[0];
    auto const maxDecodingTokens = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[1];
    auto const maxPathLen = inputDesc[getIdx(InputIdxEntry::PATHS)].dims.d[2];

    auto lastAcceptedTokenIds
        = reinterpret_cast<TokenIdType*>(outputs[getIdx(OutputIdxEntry::LAST_ACCEPTED_TOKEN_IDS)]);
    auto exclusiveSumLastAcceptedIndices
        = reinterpret_cast<SizeType32*>(outputs[getIdx(OutputIdxEntry::EXCLUSIVE_SUM_LAST_TOKEN_INDICES)]);
    auto prevDraftLens = reinterpret_cast<SizeType32 const*>(inputs[getIdx(InputIdxEntry::DRAFT_LENS)]);
    auto acceptedTokenIds = reinterpret_cast<TokenIdType const*>(outputs[getIdx(OutputIdxEntry::ACCEPTED_TOKENS)]);
    auto acceptedLengths = reinterpret_cast<SizeType32 const*>(outputs[getIdx(OutputIdxEntry::ACCEPTED_LEN)]);
    auto bestPathIds = reinterpret_cast<SizeType32 const*>(outputs[getIdx(OutputIdxEntry::BEST_ACCEPTED_PATHS)]);
    auto paths = reinterpret_cast<SizeType32 const*>(inputs[getIdx(InputIdxEntry::PATHS)]);

    invokeSelectLastAccTokenAndComputeIndicesCumSum(lastAcceptedTokenIds, exclusiveSumLastAcceptedIndices,
        prevDraftLens, acceptedTokenIds, acceptedLengths, bestPathIds, paths, batchSize, maxDecodingTokens, maxPathLen,
        stream);

    sync_check_cuda_error();

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void EagleSampleAndAcceptDraftTokensPlugin::enqueueType(nvinfer1::PluginTensorDesc const* inputDesc,
    nvinfer1::PluginTensorDesc const* outputDesc, void const* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream) noexcept
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    // TODO split batch into greedy and non-greedy and execute both paths
    if (mGreedySampling)
    {
        doGreedy<T>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
    else
    {
        // TODO fill me
        TLLM_CHECK_WITH_INFO(false, "Non-greedy sampling is not supported yet");
    }

    // Find last accepted tokens and do cumulative sum of accepted indices.
    selectLastAccTokenAndComputeIndicesCumSum(inputDesc, outputDesc, inputs, outputs, workspace, stream);

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

int EagleSampleAndAcceptDraftTokensPlugin::enqueue(nvinfer1::PluginTensorDesc const* inputDesc,
    nvinfer1::PluginTensorDesc const* outputDesc, void const* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream) noexcept
{
    auto const logitsType = inputDesc[getIdx(InputIdxEntry::LOGITS)].type;
    if (logitsType == nvinfer1::DataType::kFLOAT)
    {
        enqueueType<float>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
    else if (logitsType == nvinfer1::DataType::kHALF)
    {
        enqueueType<__half>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
    else
    {
        TLLM_CHECK_WITH_INFO(false, "Unsupported logits type");
    }

    return 0;
}

// IPluginV2Ext Methods
nvinfer1::DataType EagleSampleAndAcceptDraftTokensPlugin::getOutputDataType(
    int index, nvinfer1::DataType const* inputTypes, int nbInputs) const noexcept
{
    TLLM_CHECK(index < 7);
    // input 1 is draft tokens now of int32 type. All outputs are int32_t as well.
    return inputTypes[getIdx(InputIdxEntry::DRAFT_TOKEN_IDS)];
}

// IPluginV2 Methods

char const* EagleSampleAndAcceptDraftTokensPlugin::getPluginType() const noexcept
{
    return EAGLE_SAMPLE_AND_ACCEPT_DRAFT_TOKENS_PLUGIN_NAME;
}

char const* EagleSampleAndAcceptDraftTokensPlugin::getPluginVersion() const noexcept
{
    return EAGLE_SAMPLE_AND_ACCEPT_DRAFT_TOKENS_PLUGIN_VERSION;
}

int EagleSampleAndAcceptDraftTokensPlugin::getNbOutputs() const noexcept
{
    return 7;
}

int EagleSampleAndAcceptDraftTokensPlugin::initialize() noexcept
{
    return 0;
}

void EagleSampleAndAcceptDraftTokensPlugin::terminate() noexcept {}

size_t EagleSampleAndAcceptDraftTokensPlugin::getSerializationSize() const noexcept
{
    return sizeof(mDtype) + sizeof(mGreedySampling);
}

void EagleSampleAndAcceptDraftTokensPlugin::serialize(void* buffer) const noexcept
{
    char *d = static_cast<char*>(buffer), *a = d;
    write(d, mDtype);
    write(d, mGreedySampling);
    assert(d == a + getSerializationSize());
}

void EagleSampleAndAcceptDraftTokensPlugin::destroy() noexcept
{
    // This gets called when the network containing plugin is destroyed
    delete this;
}

///////////////

EagleSampleAndAcceptDraftTokensPluginCreator::EagleSampleAndAcceptDraftTokensPluginCreator()
{
    // Fill PluginFieldCollection with PluginField arguments metadata
    mPluginAttributes.clear();
    mPluginAttributes.emplace_back(PluginField("type_id", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("greedy_sampling", nullptr, PluginFieldType::kINT32, 1));
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

char const* EagleSampleAndAcceptDraftTokensPluginCreator::getPluginName() const noexcept
{
    return EAGLE_SAMPLE_AND_ACCEPT_DRAFT_TOKENS_PLUGIN_NAME;
}

char const* EagleSampleAndAcceptDraftTokensPluginCreator::getPluginVersion() const noexcept
{
    return EAGLE_SAMPLE_AND_ACCEPT_DRAFT_TOKENS_PLUGIN_VERSION;
}

PluginFieldCollection const* EagleSampleAndAcceptDraftTokensPluginCreator::getFieldNames() noexcept
{
    return &mFC;
}

IPluginV2* EagleSampleAndAcceptDraftTokensPluginCreator::createPlugin(
    char const* name, PluginFieldCollection const* fc) noexcept
{
    PluginField const* fields = fc->fields;
    nvinfer1::DataType type;
    bool greedySampling;
    // Read configurations from each fields
    for (int i = 0; i < fc->nbFields; ++i)
    {
        char const* attrName = fields[i].name;
        if (!strcmp(attrName, "type_id"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            type = static_cast<nvinfer1::DataType>(*(static_cast<nvinfer1::DataType const*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "greedy_sampling"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            greedySampling = static_cast<bool>(*static_cast<int32_t const*>(fields[i].data));
        }
    }

    try
    {
        auto* obj = new EagleSampleAndAcceptDraftTokensPlugin(type, greedySampling);
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (std::exception const& e)
    {
        caughtError(e);
    }
    return nullptr;
}

IPluginV2* EagleSampleAndAcceptDraftTokensPluginCreator::deserializePlugin(
    char const* name, void const* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call EagleSampleAndAcceptDraftTokensPlugin::destroy()
    try
    {
        auto* obj = new EagleSampleAndAcceptDraftTokensPlugin(serialData, serialLength);
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (std::exception const& e)
    {
        caughtError(e);
    }
    return nullptr;
}
