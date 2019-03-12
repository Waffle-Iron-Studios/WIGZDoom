
#include "vk_renderstate.h"
#include "vulkan/system/vk_framebuffer.h"
#include "vulkan/system/vk_builders.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "templates.h"
#include "doomstat.h"
#include "r_data/colormaps.h"
#include "hwrenderer/scene/hw_skydome.h"
#include "hwrenderer/scene/hw_viewpointuniforms.h"
#include "hwrenderer/dynlights/hw_lightbuffer.h"
#include "hwrenderer/utility/hw_cvars.h"
#include "hwrenderer/utility/hw_clock.h"
#include "hwrenderer/data/flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"

VkRenderState::VkRenderState()
{
	mIdentityMatrix.loadIdentity();
	Reset();
}

void VkRenderState::ClearScreen()
{
	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	Apply(DT_TriangleStrip);
	mCommandBuffer->draw(4, 1, FFlatVertexBuffer::FULLSCREEN_INDEX, 0);
}

void VkRenderState::Draw(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);

	drawcalls.Clock();
	mCommandBuffer->draw(count, 1, index, 0);
	drawcalls.Unclock();
}

void VkRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);

	drawcalls.Clock();
	mCommandBuffer->drawIndexed(count, 1, index, 0, 0);
	drawcalls.Unclock();
}

bool VkRenderState::SetDepthClamp(bool on)
{
	bool lastValue = mDepthClamp;
	mDepthClamp = on;
	mNeedApply = true;
	return lastValue;
}

void VkRenderState::SetDepthMask(bool on)
{
	mDepthWrite = on;
	mNeedApply = true;
}

void VkRenderState::SetDepthFunc(int func)
{
	mDepthFunc = func;
	mNeedApply = true;
}

void VkRenderState::SetDepthRange(float min, float max)
{
	mViewportDepthMin = min;
	mViewportDepthMax = max;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	int rr = r, gg = g, bb = b, aa = a;
	mColorMask = (aa << 3) | (bb << 2) | (gg << 1) | rr;
	mNeedApply = true;
}

void VkRenderState::EnableDrawBufferAttachments(bool on)
{
}

void VkRenderState::SetStencil(int offs, int op, int flags)
{
	mStencilRef = screen->stencilValue + offs;
	mStencilRefChanged = true;
	mStencilOp = op;

	if (flags != -1)
	{
		bool cmon = !(flags & SF_ColorMaskOff);
		SetColorMask(cmon, cmon, cmon, cmon); // don't write to the graphics buffer
		mDepthWrite = !(flags & SF_DepthMaskOff);
	}

	mNeedApply = true;
}

void VkRenderState::SetCulling(int mode)
{
	mCullMode = mode;
	mNeedApply = true;
}

void VkRenderState::EnableClipDistance(int num, bool state)
{
}

void VkRenderState::Clear(int targets)
{
	// We need an active render pass, and it must have a depth attachment..
	bool lastDepthTest = mDepthTest;
	bool lastDepthWrite = mDepthWrite;
	if (targets & (CT_Depth | CT_Stencil))
	{
		mDepthTest = true;
		mDepthWrite = true;
	}
	Apply(DT_TriangleStrip);
	mDepthTest = lastDepthTest;
	mDepthWrite = lastDepthWrite;

	VkClearAttachment attachments[2] = { };
	VkClearRect rects[2] = { };

	for (int i = 0; i < 2; i++)
	{
		rects[i].layerCount = 1;
		if (mScissorWidth >= 0)
		{
			rects[0].rect.offset.x = mScissorX;
			rects[0].rect.offset.y = GetVulkanFrameBuffer()->GetBuffers()->GetHeight() - mScissorY - mViewportHeight;
			rects[0].rect.extent.width = mScissorWidth;
			rects[0].rect.extent.height = mScissorHeight;

			if (rects[0].rect.offset.x < 0)
			{
				rects[0].rect.extent.height += rects[0].rect.offset.x;
				rects[0].rect.offset.x = 0;
			}
			if (rects[0].rect.offset.y < 0)
			{
				rects[0].rect.extent.height += rects[0].rect.offset.y;
				rects[0].rect.offset.y = 0;
			}
		}
		else
		{
			rects[0].rect.offset.x = 0;
			rects[0].rect.offset.y = 0;
			rects[0].rect.extent.width = SCREENWIDTH;
			rects[0].rect.extent.height = SCREENHEIGHT;
		}
	}

	if (targets & CT_Depth)
	{
		attachments[1].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		attachments[1].clearValue.depthStencil.depth = 1.0f;
	}
	if (targets & CT_Stencil)
	{
		attachments[1].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		attachments[1].clearValue.depthStencil.stencil = 0;
	}
	if (targets & CT_Color)
	{
		attachments[0].aspectMask |= VK_IMAGE_ASPECT_COLOR_BIT;
		for (int i = 0; i < 4; i++)
			attachments[0].clearValue.color.float32[i] = screen->mSceneClearColor[i];
	}

	if ((targets & CT_Color) && (targets & CT_Stencil) && (targets & CT_Depth))
		mCommandBuffer->clearAttachments(2, attachments, 2, rects);
	else if (targets & (CT_Stencil | CT_Depth))
		mCommandBuffer->clearAttachments(1, attachments + 1, 1, rects + 1);
	else if (targets & CT_Color)
		mCommandBuffer->clearAttachments(1, attachments, 1, rects);
}

void VkRenderState::EnableStencil(bool on)
{
	mStencilTest = on;
	mNeedApply = true;
}

void VkRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
	mScissorChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::EnableDepthTest(bool on)
{
	mDepthTest = on;
	mNeedApply = true;
}

void VkRenderState::EnableMultisampling(bool on)
{
}

void VkRenderState::EnableLineSmooth(bool on)
{
}

void VkRenderState::Apply(int dt)
{
	ApplyRenderPass(dt);
	ApplyScissor();
	ApplyViewport();
	ApplyStencilRef();
	ApplyDepthBias();
	ApplyStreamData();
	ApplyMatrices();
	ApplyPushConstants();
	ApplyVertexBuffers();
	ApplyDynamicSet();
	ApplyMaterial();
	mNeedApply = false;
}

void VkRenderState::ApplyDepthBias()
{
	if (mBias.mChanged)
	{
		mCommandBuffer->setDepthBias(mBias.mUnits, 0.0f, mBias.mFactor);
		mBias.mChanged = false;
	}
}

void VkRenderState::ApplyRenderPass(int dt)
{
	auto passManager = GetVulkanFrameBuffer()->GetRenderPassManager();

	// Find a render pass that matches our state
	VkRenderPassKey passKey;
	passKey.DrawType = dt;
	passKey.VertexFormat = static_cast<VKVertexBuffer*>(mVertexBuffer)->VertexFormat;
	passKey.RenderStyle = mRenderStyle;
	passKey.DepthTest = mDepthTest;
	passKey.DepthWrite = mDepthTest && mDepthWrite;
	passKey.DepthFunc = mDepthFunc;
	passKey.DepthClamp = mDepthClamp;
	passKey.DepthBias = !(mBias.mFactor == 0 && mBias.mUnits == 0);
	passKey.StencilTest = mStencilTest;
	passKey.StencilPassOp = mStencilOp;
	passKey.ColorMask = mColorMask;
	passKey.CullMode = mCullMode;
	passKey.Samples = passManager->GetSamples();
	if (mSpecialEffect > EFF_NONE)
	{
		passKey.SpecialEffect = mSpecialEffect;
		passKey.EffectState = 0;
		passKey.AlphaTest = false;
	}
	else
	{
		int effectState = mMaterial.mOverrideShader >= 0 ? mMaterial.mOverrideShader : (mMaterial.mMaterial ? mMaterial.mMaterial->GetShaderIndex() : 0);
		passKey.SpecialEffect = EFF_NONE;
		passKey.EffectState = mTextureEnabled ? effectState : SHADER_NoTexture;
		passKey.AlphaTest = mAlphaThreshold >= 0.f;
	}

	// Is this the one we already have or do we need to change render pass?
	bool changingRenderPass = (passKey != mRenderPassKey);

	if (!mCommandBuffer)
	{
		mCommandBuffer = GetVulkanFrameBuffer()->GetDrawCommands();
		changingRenderPass = true;
		mScissorChanged = true;
		mViewportChanged = true;
		mStencilRefChanged = true;
		mBias.mChanged = true;
	}
	else if (changingRenderPass)
	{
		mCommandBuffer->endRenderPass();
	}

	if (changingRenderPass)
	{
		passManager->BeginRenderPass(passKey, mCommandBuffer);
		mRenderPassKey = passKey;
	}
}

void VkRenderState::ApplyStencilRef()
{
	if (mStencilRefChanged)
	{
		mCommandBuffer->setStencilReference(VK_STENCIL_FRONT_AND_BACK, mStencilRef);
		mStencilRefChanged = false;
	}
}

void VkRenderState::ApplyScissor()
{
	if (mScissorChanged)
	{
		VkRect2D scissor;
		auto buffers = GetVulkanFrameBuffer()->GetBuffers();
		if (mScissorWidth >= 0)
		{
			scissor.offset.x = mScissorX;
			scissor.offset.y = buffers->GetHeight() - mScissorY - mViewportHeight;
			scissor.extent.width = mScissorWidth;
			scissor.extent.height = mScissorHeight;

			if (scissor.offset.x < 0)
			{
				scissor.extent.height += scissor.offset.x;
				scissor.offset.x = 0;
			}
			if (scissor.offset.y < 0)
			{
				scissor.extent.height += scissor.offset.y;
				scissor.offset.y = 0;
			}
		}
		else
		{
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent.width = buffers->GetWidth();
			scissor.extent.height = buffers->GetHeight();
		}
		mCommandBuffer->setScissor(0, 1, &scissor);
		mScissorChanged = false;
	}
}

void VkRenderState::ApplyViewport()
{
	if (mViewportChanged)
	{
		VkViewport viewport;
		auto buffers = GetVulkanFrameBuffer()->GetBuffers();
		if (mViewportWidth >= 0)
		{
			viewport.x = (float)mViewportX;
			viewport.y = (float)buffers->GetHeight() - mViewportY - mViewportHeight;
			viewport.width = (float)mViewportWidth;
			viewport.height = (float)mViewportHeight;
		}
		else
		{
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = (float)buffers->GetWidth();
			viewport.height = (float)buffers->GetHeight();
		}
		viewport.minDepth = mViewportDepthMin;
		viewport.maxDepth = mViewportDepthMax;
		mCommandBuffer->setViewport(0, 1, &viewport);
		mViewportChanged = false;
	}
}

void VkRenderState::ApplyStreamData()
{
	auto fb = GetVulkanFrameBuffer();
	auto passManager = fb->GetRenderPassManager();

	const float normScale = 1.0f / 255.0f;

	mStreamData.uDesaturationFactor = mDesaturation * normScale;
	mStreamData.uFogColor = { mFogColor.r * normScale, mFogColor.g * normScale, mFogColor.b * normScale, mFogColor.a * normScale };
	mStreamData.uAddColor = { mAddColor.r * normScale, mAddColor.g * normScale, mAddColor.b * normScale, mAddColor.a * normScale };
	mStreamData.uObjectColor = { mObjectColor.r * normScale, mObjectColor.g * normScale, mObjectColor.b * normScale, mObjectColor.a * normScale };
	mStreamData.uDynLightColor = mDynColor.vec;
	mStreamData.uInterpolationFactor = mInterpolationFactor;

	mStreamData.useVertexData = passManager->VertexFormats[static_cast<VKVertexBuffer*>(mVertexBuffer)->VertexFormat].UseVertexData;
	mStreamData.uVertexColor = mColor.vec;
	mStreamData.uVertexNormal = mNormal.vec;

	mStreamData.timer = 0.0f; // static_cast<float>((double)(screen->FrameTime - firstFrame) * (double)mShaderTimer / 1000.);

	if (mGlowEnabled)
	{
		mStreamData.uGlowTopPlane = mGlowTopPlane.vec;
		mStreamData.uGlowTopColor = mGlowTop.vec;
		mStreamData.uGlowBottomPlane = mGlowBottomPlane.vec;
		mStreamData.uGlowBottomColor = mGlowBottom.vec;
		mLastGlowEnabled = true;
	}
	else if (mLastGlowEnabled)
	{
		mStreamData.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mLastGlowEnabled = false;
	}

	if (mGradientEnabled)
	{
		mStreamData.uObjectColor2 = { mObjectColor2.r * normScale, mObjectColor2.g * normScale, mObjectColor2.b * normScale, mObjectColor2.a * normScale };
		mStreamData.uGradientTopPlane = mGradientTopPlane.vec;
		mStreamData.uGradientBottomPlane = mGradientBottomPlane.vec;
		mLastGradientEnabled = true;
	}
	else if (mLastGradientEnabled)
	{
		mStreamData.uObjectColor2 = { 0.0f, 0.0f, 0.0f, 0.0f };
		mLastGradientEnabled = false;
	}

	if (mSplitEnabled)
	{
		mStreamData.uSplitTopPlane = mSplitTopPlane.vec;
		mStreamData.uSplitBottomPlane = mSplitBottomPlane.vec;
		mLastSplitEnabled = true;
	}
	else if (mLastSplitEnabled)
	{
		mStreamData.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mLastSplitEnabled = false;
	}

	mDataIndex++;
	if (mDataIndex == MAX_STREAM_DATA)
	{
		mDataIndex = 0;
		mStreamDataOffset += sizeof(StreamUBO);
	}
	uint8_t *ptr = (uint8_t*)fb->StreamUBO->Memory();
	memcpy(ptr + mStreamDataOffset + sizeof(StreamData) * mDataIndex, &mStreamData, sizeof(StreamData));
}

void VkRenderState::ApplyPushConstants()
{
	int fogset = 0;
	if (mFogEnabled)
	{
		if (mFogEnabled == 2)
		{
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		}
		else if ((mFogColor & 0xffffff) == 0)
		{
			fogset = gl_fogmode;
		}
		else
		{
			fogset = -gl_fogmode;
		}
	}

	int tempTM = TM_NORMAL;
	if (mMaterial.mMaterial && mMaterial.mMaterial->tex->isHardwareCanvas())
		tempTM = TM_OPAQUE;

	mPushConstants.uFogEnabled = fogset;
	mPushConstants.uTextureMode = mTextureMode == TM_NORMAL && tempTM == TM_OPAQUE ? TM_OPAQUE : mTextureMode;
	mPushConstants.uLightDist = mLightParms[0];
	mPushConstants.uLightFactor = mLightParms[1];
	mPushConstants.uFogDensity = mLightParms[2];
	mPushConstants.uLightLevel = mLightParms[3];
	mPushConstants.uAlphaThreshold = mAlphaThreshold;
	mPushConstants.uClipSplit = { mClipSplit[0], mClipSplit[1] };

	//if (mMaterial.mMaterial)
	//	mPushConstants.uSpecularMaterial = { mMaterial.mMaterial->tex->Glossiness, mMaterial.mMaterial->tex->SpecularLevel };

	mPushConstants.uLightIndex = screen->mLights->BindUBO(mLightIndex);
	mPushConstants.uDataIndex = mDataIndex;

	auto fb = GetVulkanFrameBuffer();
	auto passManager = fb->GetRenderPassManager();
	mCommandBuffer->pushConstants(passManager->PipelineLayout.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(PushConstants), &mPushConstants);
}

template<typename T>
static void BufferedSet(bool &modified, T &dst, const T &src)
{
	if (dst == src)
		return;
	dst = src;
	modified = true;
}

static void BufferedSet(bool &modified, VSMatrix &dst, const VSMatrix &src)
{
	if (memcmp(dst.get(), src.get(), sizeof(FLOATTYPE) * 16) == 0)
		return;
	dst = src;
	modified = true;
}

void VkRenderState::ApplyMatrices()
{
	bool modified = (mMatricesOffset == 0); // always modified first call
	if (mTextureMatrixEnabled)
	{
		BufferedSet(modified, mMatrices.TextureMatrix, mTextureMatrix);
	}
	else
	{
		BufferedSet(modified, mMatrices.TextureMatrix, mIdentityMatrix);
	}

	if (mModelMatrixEnabled)
	{
		BufferedSet(modified, mMatrices.ModelMatrix, mModelMatrix);
		if (modified)
			mMatrices.NormalModelMatrix.computeNormalMatrix(mModelMatrix);
	}
	else
	{
		BufferedSet(modified, mMatrices.ModelMatrix, mIdentityMatrix);
		BufferedSet(modified, mMatrices.NormalModelMatrix, mIdentityMatrix);
	}

	if (modified)
	{
		auto fb = GetVulkanFrameBuffer();

		if (mMatricesOffset + (fb->UniformBufferAlignedSize<MatricesUBO>() << 1) < fb->MatricesUBO->Size())
		{
			mMatricesOffset += fb->UniformBufferAlignedSize<MatricesUBO>();
			memcpy(static_cast<uint8_t*>(fb->MatricesUBO->Memory()) + mMatricesOffset, &mMatrices, sizeof(MatricesUBO));
		}
	}
}

void VkRenderState::ApplyVertexBuffers()
{
	if (mVertexBuffer != mLastVertexBuffer && mVertexBuffer)
	{
		VkBuffer vertexBuffers[] = { static_cast<VKVertexBuffer*>(mVertexBuffer)->mBuffer->buffer };
		VkDeviceSize offsets[] = { 0 };
		mCommandBuffer->bindVertexBuffers(0, 1, vertexBuffers, offsets);
		mLastVertexBuffer = mVertexBuffer;
	}

	if (mIndexBuffer != mLastIndexBuffer && mIndexBuffer)
	{
		mCommandBuffer->bindIndexBuffer(static_cast<VKIndexBuffer*>(mIndexBuffer)->mBuffer->buffer, 0, VK_INDEX_TYPE_UINT32);
		mLastIndexBuffer = mIndexBuffer;
	}
}

void VkRenderState::ApplyMaterial()
{
	if (mMaterial.mChanged && mMaterial.mMaterial)
	{
		auto base = static_cast<VkHardwareTexture*>(mMaterial.mMaterial->GetLayer(0, mMaterial.mTranslation));
		if (base)
		{
			auto fb = GetVulkanFrameBuffer();
			auto passManager = fb->GetRenderPassManager();
			mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->PipelineLayout.get(), 1, base->GetDescriptorSet(mMaterial));
		}

		mMaterial.mChanged = false;
	}
}

void VkRenderState::ApplyDynamicSet()
{
	if (mViewpointOffset != mLastViewpointOffset || mLightBufferOffset != mLastLightBufferOffset || mMatricesOffset != mLastMatricesOffset || mStreamDataOffset != mLastStreamDataOffset)
	{
		auto fb = GetVulkanFrameBuffer();
		auto passManager = fb->GetRenderPassManager();

		uint32_t offsets[4] = { mViewpointOffset, mLightBufferOffset, mMatricesOffset, mStreamDataOffset };
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->PipelineLayout.get(), 0, passManager->DynamicSet.get(), 4, offsets);

		mLastViewpointOffset = mViewpointOffset;
		mLastLightBufferOffset = mLightBufferOffset;
		mLastMatricesOffset = mMatricesOffset;
		mLastStreamDataOffset = mStreamDataOffset;
	}
}

void VkRenderState::Bind(int bindingpoint, uint32_t offset)
{
	if (bindingpoint == VIEWPOINT_BINDINGPOINT)
	{
		mViewpointOffset = offset;
		mNeedApply = true;
	}
	else if (bindingpoint == LIGHTBUF_BINDINGPOINT)
	{
		mLightBufferOffset = offset;
		mNeedApply = true;
	}
}

void VkRenderState::EndRenderPass()
{
	if (mCommandBuffer)
	{
		mCommandBuffer->endRenderPass();
		mCommandBuffer = nullptr;
		mRenderPassKey = {};

		mLastViewpointOffset = 0xffffffff;
		mLastLightBufferOffset = 0xffffffff;
		mLastVertexBuffer = nullptr;
		mLastIndexBuffer = nullptr;
		mLastGlowEnabled = true;
		mLastGradientEnabled = true;
		mLastSplitEnabled = true;
		mLastModelMatrixEnabled = true;
		mLastTextureMatrixEnabled = true;
	}
}

void VkRenderState::EndFrame()
{
	mMatricesOffset = 0;
	mStreamDataOffset = 0;
	mDataIndex = -1;
}

/////////////////////////////////////////////////////////////////////////////

void VkRenderStateMolten::Draw(int dt, int index, int count, bool apply)
{
	if (dt == DT_TriangleFan)
	{
		IIndexBuffer *oldIndexBuffer = mIndexBuffer;
		mIndexBuffer = GetVulkanFrameBuffer()->FanToTrisIndexBuffer.get();

		if (apply || mNeedApply)
			Apply(DT_Triangles);
		else
			ApplyVertexBuffers();

		drawcalls.Clock();
		mCommandBuffer->drawIndexed((count - 2) * 3, 1, 0, index, 0);
		drawcalls.Unclock();

		mIndexBuffer = oldIndexBuffer;
	}
	else
	{
		if (apply || mNeedApply)
			Apply(dt);

		drawcalls.Clock();
		mCommandBuffer->draw(count, 1, index, 0);
		drawcalls.Unclock();
	}
}
