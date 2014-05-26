#include "CmD3D9RenderSystem.h"
#include "CmD3D9Prerequisites.h"
#include "CmD3D9DriverList.h"
#include "CmD3D9Driver.h"
#include "CmD3D9RenderWindow.h"
#include "CmD3D9TextureManager.h"
#include "CmD3D9Texture.h"
#include "CmMath.h"
#include "CmD3D9HardwareBufferManager.h"
#include "CmD3D9IndexBuffer.h"
#include "CmD3D9VertexBuffer.h"
#include "CmD3D9VertexDeclaration.h"
#include "CmD3D9GpuProgram.h"
#include "CmD3D9HLSLProgramFactory.h"
#include "CmD3D9OcclusionQuery.h"
#include "CmD3D9DeviceManager.h"
#include "CmD3D9ResourceManager.h"
#include "CmD3D9RenderWindowManager.h"
#include "CmGpuProgramManager.h"
#include "CmRenderStateManager.h"
#include "CmAsyncOp.h"
#include "CmBlendState.h"
#include "CmRasterizerState.h"
#include "CmDepthStencilState.h"
#include "CmGpuParams.h"
#include "CmGpuParamDesc.h"
#include "CmGpuParamBlockBuffer.h"
#include "CmCoreThread.h"
#include "CmD3D9QueryManager.h"
#include "CmDebug.h"

#define FLOAT2DWORD(f) *((DWORD*)&f)

// Formats to try, in decreasing order of preference
D3DFORMAT ddDepthStencilFormats[]={
	D3DFMT_D24FS8,
	D3DFMT_D24S8,
	D3DFMT_D24X4S4,
	D3DFMT_D24X8,
	D3DFMT_D15S1,
	D3DFMT_D16,
	D3DFMT_D32
};
#define NDSFORMATS (sizeof(ddDepthStencilFormats)/sizeof(D3DFORMAT))

namespace BansheeEngine 
{
	D3D9RenderSystem* D3D9RenderSystem::msD3D9RenderSystem = NULL;

	D3D9RenderSystem::D3D9RenderSystem( HINSTANCE hInstance )
		: mTexStageDesc(nullptr), mNumTexStages(0), mCurrentDrawOperation(DOT_TRIANGLE_LIST), 
		mViewportLeft(0), mViewportTop(0), mViewportWidth(0), mViewportHeight(0),
		mIsFrameInProgress(false), mRestoreFrameOnReset(false)
	{
		// update singleton access pointer.
		msD3D9RenderSystem = this;

		// set the instance being passed 
		mhInstance = hInstance;

		// set pointers to NULL
		mpD3D = NULL;		
		mDriverList = NULL;
		mActiveD3DDriver = NULL;	
		mHLSLProgramFactory = NULL;		
		mDeviceManager = NULL;	
		mResourceManager = nullptr;	

		mScissorRect.left = 0;
		mScissorRect.right = 1280;
		mScissorRect.top = 0;
		mScissorRect.bottom = 720;
	}

	D3D9RenderSystem::~D3D9RenderSystem()
	{

	}

	const String& D3D9RenderSystem::getName() const
	{
		static String strName( "D3D9RenderSystem");
		return strName;
	}

	const String& D3D9RenderSystem::getShadingLanguageName() const
	{
		static String strName("hlsl");
		return strName;
	}

	void D3D9RenderSystem::initialize_internal(AsyncOp& asyncOp)
	{
		THROW_IF_NOT_CORE_THREAD;

		// Create the resource manager.
		mResourceManager = cm_new<D3D9ResourceManager>();

		// Create our Direct3D object
		if( NULL == (mpD3D = Direct3DCreate9(D3D_SDK_VERSION)) )
			CM_EXCEPT(InternalErrorException, "Failed to create Direct3D9 object");

		RenderWindow* autoWindow = NULL;

		// Init using current settings
		mActiveD3DDriver = getDirect3DDrivers()->item(0); // TODO - We always use the first driver

		if( !mActiveD3DDriver )
			CM_EXCEPT(InvalidParametersException, "Problems finding requested Direct3D driver!" );

		// get driver version
		mDriverVersion.major = HIWORD(mActiveD3DDriver->getAdapterIdentifier().DriverVersion.HighPart);
		mDriverVersion.minor = LOWORD(mActiveD3DDriver->getAdapterIdentifier().DriverVersion.HighPart);
		mDriverVersion.release = HIWORD(mActiveD3DDriver->getAdapterIdentifier().DriverVersion.LowPart);
		mDriverVersion.build = LOWORD(mActiveD3DDriver->getAdapterIdentifier().DriverVersion.LowPart);

		mVideoModeInfo = getDirect3DDrivers()->getVideoModeInfo();

		// Create the device manager.
		mDeviceManager = cm_new<D3D9DeviceManager>();

		// Also create hardware buffer manager		
		HardwareBufferManager::startUp<D3D9HardwareBufferManager>();

		// Create & register HLSL factory		
		mHLSLProgramFactory = cm_new<D3D9HLSLProgramFactory>();

		// Create render window manager
		RenderWindowManager::startUp<D3D9RenderWindowManager>(this);

		// Create render state manager
		RenderStateManager::startUp();

		// Create primary window and finalize initialization
		RenderWindowPtr primaryWindow = RenderWindow::create(mPrimaryWindowDesc);
		D3D9RenderWindow* d3d9renderWindow = static_cast<D3D9RenderWindow*>(primaryWindow.get());
		updateRenderSystemCapabilities(d3d9renderWindow);

		// Create the texture manager for use by others		
		TextureManager::startUp<D3D9TextureManager>();

		QueryManager::startUp<D3D9QueryManager>();

		// call superclass method
		RenderSystem::initialize_internal(asyncOp);

		asyncOp._completeOperation(primaryWindow);
	}

	void D3D9RenderSystem::destroy_internal()
	{
		if(mTexStageDesc != nullptr)
		{
			cm_deleteN(mTexStageDesc, mNumTexStages);
			mTexStageDesc = nullptr;
		}

		RenderSystem::destroy_internal();

		if(mDeviceManager != nullptr)
		{
			cm_delete(mDeviceManager);
			mDeviceManager = nullptr;
		}

		if(mDriverList != nullptr)
		{
			cm_delete(mDriverList);
			mDriverList = nullptr;
		}
		mActiveD3DDriver = NULL;	

		QueryManager::shutDown();
		TextureManager::shutDown();
		HardwareBufferManager::shutDown();
		RenderWindowManager::shutDown();
		RenderStateManager::shutDown();

		// Deleting the HLSL program factory
		if (mHLSLProgramFactory)
		{
			GpuProgramManager::instance().removeFactory(mHLSLProgramFactory);
			cm_delete(mHLSLProgramFactory);
			mHLSLProgramFactory = 0;
		}

		SAFE_RELEASE(mpD3D);

		if(mResourceManager != nullptr)
		{
			cm_delete(mResourceManager);
			mResourceManager = nullptr;
		}

		msD3D9RenderSystem = NULL;
	}

	void D3D9RenderSystem::registerWindow(RenderWindow& renderWindow)
	{		
		THROW_IF_NOT_CORE_THREAD;

		D3D9RenderWindow* d3d9renderWindow = static_cast<D3D9RenderWindow*>(&renderWindow);

		String msg;

		mResourceManager->lockDeviceAccess();

		try
		{
			mDeviceManager->linkRenderWindow(d3d9renderWindow);
		}
		catch (const BansheeEngine::RenderingAPIException&)
		{
			// after catching the exception, clean up
			mResourceManager->unlockDeviceAccess();

			// re-throw
			throw;
		}

		mResourceManager->unlockDeviceAccess();
	}	

	void D3D9RenderSystem::bindGpuProgram(HGpuProgram prg)
	{
		THROW_IF_NOT_CORE_THREAD;

		if(!prg.isLoaded())
			return;

		GpuProgramPtr bindingPrg = std::static_pointer_cast<GpuProgram>(prg->getThisPtr());

		HRESULT hr;
		switch (bindingPrg->getType())
		{
		case GPT_VERTEX_PROGRAM:
			hr = getActiveD3D9Device()->SetVertexShader(
				static_cast<D3D9GpuVertexProgram*>(bindingPrg.get())->getVertexShader());
			if (FAILED(hr))
			{
				CM_EXCEPT(RenderingAPIException, "Error calling SetVertexShader");
			}
			break;
		case GPT_FRAGMENT_PROGRAM:
			hr = getActiveD3D9Device()->SetPixelShader(
				static_cast<D3D9GpuFragmentProgram*>(bindingPrg.get())->getPixelShader());
			if (FAILED(hr))
			{
				CM_EXCEPT(RenderingAPIException, "Error calling SetPixelShader");
			}
			break;
		};

		// Make sure texcoord index is equal to stage value, As SDK Doc suggests:
		// "When rendering using vertex shaders, each stage's texture coordinate index must be set to its default value."
		// This solves such an errors when working with the Debug runtime -
		// "Direct3D9: (ERROR) :Stage 1 - Texture coordinate index in the stage must be equal to the stage index when programmable vertex pipeline is used".
		for (unsigned int nStage=0; nStage < 8; ++nStage)
			__SetTextureStageState(nStage, D3DTSS_TEXCOORDINDEX, nStage);

		mRenderStats.numGpuProgramBinds++;

		RenderSystem::bindGpuProgram(prg);
	}

	void D3D9RenderSystem::unbindGpuProgram(GpuProgramType gptype)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		switch(gptype)
		{
		case GPT_VERTEX_PROGRAM:
			hr = getActiveD3D9Device()->SetVertexShader(NULL);
			if (FAILED(hr))
			{
				CM_EXCEPT(RenderingAPIException, "Error resetting SetVertexShader to NULL");
			}
			break;
		case GPT_FRAGMENT_PROGRAM:
			hr = getActiveD3D9Device()->SetPixelShader(NULL);
			if (FAILED(hr))
			{
				CM_EXCEPT(RenderingAPIException, "Error resetting SetPixelShader to NULL");
			}
			break;
		};

		mRenderStats.numGpuProgramBinds++;

		RenderSystem::unbindGpuProgram(gptype);
	}

	void D3D9RenderSystem::bindGpuParams(GpuProgramType gptype, BindableGpuParams& bindableParams)
	{
		THROW_IF_NOT_CORE_THREAD;

		bindableParams.updateHardwareBuffers();
		const GpuParamDesc& paramDesc = bindableParams.getParamDesc();

		for(auto iter = paramDesc.samplers.begin(); iter != paramDesc.samplers.end(); ++iter)
		{
			HSamplerState& samplerState = bindableParams.getSamplerState(iter->second.slot);

			if(samplerState == nullptr)
				setSamplerState(gptype, iter->second.slot, SamplerState::getDefault());
			else
				setSamplerState(gptype, iter->second.slot, samplerState.getInternalPtr());
		}

		for(auto iter = paramDesc.textures.begin(); iter != paramDesc.textures.end(); ++iter)
		{
			HTexture texture = bindableParams.getTexture(iter->second.slot);

			if(!texture.isLoaded())
				setTexture(gptype, iter->second.slot, false, nullptr);
			else
				setTexture(gptype, iter->second.slot, true, texture.getInternalPtr());
		}

		// Read all the buffer data so we can assign it. Not the most efficient way of accessing data
		// but it is required in order to have standardized buffer interface.
		UnorderedMap<UINT32, UINT8*> bufferData;

		for(auto& curParam : paramDesc.params)
		{
			UINT32 paramBlockSlot = curParam.second.paramBlockSlot;
			auto iterFind = bufferData.find(paramBlockSlot);

			if(iterFind == bufferData.end())
			{
				GpuParamBlockBufferPtr paramBlock = bindableParams.getParamBlockBuffer(paramBlockSlot);

				UINT8* data = (UINT8*)cm_alloc<ScratchAlloc>(paramBlock->getSize());
				paramBlock->readData(data);

				bufferData[paramBlockSlot] = data;
			}
		}

		HRESULT hr;

		switch(gptype)
		{
		case GPT_VERTEX_PROGRAM:
			{
				for(auto iter = paramDesc.params.begin(); iter != paramDesc.params.end(); ++iter)
				{
					const GpuParamDataDesc& paramDesc = iter->second;

					const UINT8* ptrData = bufferData[paramDesc.paramBlockSlot] + paramDesc.cpuMemOffset * sizeof(UINT32);

					switch(paramDesc.type)
					{
					case GPDT_FLOAT1:
					case GPDT_FLOAT2:
					case GPDT_FLOAT3:
					case GPDT_FLOAT4:
					case GPDT_MATRIX_2X2:
					case GPDT_MATRIX_2X3:
					case GPDT_MATRIX_2X4:
					case GPDT_MATRIX_3X2:
					case GPDT_MATRIX_3X3:
					case GPDT_MATRIX_3X4:
					case GPDT_MATRIX_4X2:
					case GPDT_MATRIX_4X3:
					case GPDT_MATRIX_4X4:
						{
							UINT32 slotCount = (paramDesc.elementSize / 4) * paramDesc.arraySize;
							assert (paramDesc.elementSize % 4 == 0 && "Should not have any elements less than 4 wide for D3D9");

							if (FAILED(hr = getActiveD3D9Device()->SetVertexShaderConstantF(paramDesc.gpuMemOffset, (const float*)ptrData, slotCount))) 
								CM_EXCEPT(RenderingAPIException, "Unable to upload vertex shader float parameters.");
							break;
						}
					case GPDT_INT1:
					case GPDT_INT2:
					case GPDT_INT3:
					case GPDT_INT4:
						{
							UINT32 slotCount = (paramDesc.elementSize / 4) * paramDesc.arraySize;
							assert (paramDesc.elementSize % 4 == 0 && "Should not have any elements less than 4 wide for D3D9");

							if (FAILED(hr = getActiveD3D9Device()->SetVertexShaderConstantI(paramDesc.gpuMemOffset, (const INT32*)ptrData, slotCount))) 
								CM_EXCEPT(RenderingAPIException, "Unable to upload vertex shader int parameters.");
							break;
						}
					case GPDT_BOOL:
						if (FAILED(hr = getActiveD3D9Device()->SetVertexShaderConstantB(paramDesc.gpuMemOffset, (const BOOL*)ptrData, paramDesc.arraySize))) 
							CM_EXCEPT(RenderingAPIException, "Unable to upload vertex shader bool parameters.");
						break;
					}
				}
			}
			break;
		case GPT_FRAGMENT_PROGRAM:
			{
				for(auto iter = paramDesc.params.begin(); iter != paramDesc.params.end(); ++iter)
				{
					const GpuParamDataDesc& paramDesc = iter->second;

					const UINT8* ptrData = bufferData[paramDesc.paramBlockSlot] + paramDesc.cpuMemOffset * sizeof(UINT32);

					switch(paramDesc.type)
					{
					case GPDT_FLOAT1:
					case GPDT_FLOAT2:
					case GPDT_FLOAT3:
					case GPDT_FLOAT4:
					case GPDT_MATRIX_2X2:
					case GPDT_MATRIX_2X3:
					case GPDT_MATRIX_2X4:
					case GPDT_MATRIX_3X2:
					case GPDT_MATRIX_3X3:
					case GPDT_MATRIX_3X4:
					case GPDT_MATRIX_4X2:
					case GPDT_MATRIX_4X3:
					case GPDT_MATRIX_4X4:
						{
							UINT32 slotCount = (paramDesc.elementSize / 4) * paramDesc.arraySize;
							assert (paramDesc.elementSize % 4 == 0 && "Should not have any elements less than 4 wide for D3D9");

							if (FAILED(hr = getActiveD3D9Device()->SetPixelShaderConstantF(paramDesc.gpuMemOffset, (const float*)ptrData, slotCount))) 
								CM_EXCEPT(RenderingAPIException, "Unable to upload pixel shader float parameters.");
							break;
						}
					case GPDT_INT1:
					case GPDT_INT2:
					case GPDT_INT3:
					case GPDT_INT4:
						{
							UINT32 slotCount = (paramDesc.elementSize / 4) * paramDesc.arraySize;
							assert (paramDesc.elementSize % 4 == 0 && "Should not have any elements less than 4 wide for D3D9");

							if (FAILED(hr = getActiveD3D9Device()->SetPixelShaderConstantI(paramDesc.gpuMemOffset, (const INT32*)ptrData, slotCount))) 
								CM_EXCEPT(RenderingAPIException, "Unable to upload pixel shader int parameters.");
							break;
						}
					case GPDT_BOOL:
						if (FAILED(hr = getActiveD3D9Device()->SetPixelShaderConstantB(paramDesc.gpuMemOffset, (const BOOL*)ptrData, paramDesc.arraySize))) 
							CM_EXCEPT(RenderingAPIException, "Unable to upload pixel shader bool parameters.");
						break;
					}
				}
			}
			break;
		};

		for(auto& curBufferData : bufferData)
		{
			cm_free<ScratchAlloc>(curBufferData.second);
		}

		mRenderStats.numGpuParamBufferBinds++;
	}

	void D3D9RenderSystem::setTexture(GpuProgramType gptype, UINT16 unit, bool enabled, const TexturePtr& tex)
	{
		THROW_IF_NOT_CORE_THREAD;

		if(!tex->isBindableAsShaderResource())
			CM_EXCEPT(InvalidParametersException, "Texture you have specified cannot be bound to a shader.");

		if(gptype != GPT_FRAGMENT_PROGRAM && gptype != GPT_VERTEX_PROGRAM)
		{
			LOGWRN("D3D9 cannot assign textures to this gpu program type: " + toString(gptype));
			return;
		}

		if(gptype == GPT_VERTEX_PROGRAM)
		{
			unit = D3DVERTEXTEXTURESAMPLER0 + unit; // Vertex stage uses special samplers
		}

		HRESULT hr;
		D3D9TexturePtr dt = std::static_pointer_cast<D3D9Texture>(tex);
		if (enabled && (dt != nullptr))
		{
			IDirect3DBaseTexture9 *pTex = dt->getTexture_internal();
			if (mTexStageDesc[unit].pTex != pTex)
			{
				hr = getActiveD3D9Device()->SetTexture(static_cast<DWORD>(unit), pTex);
				if( hr != S_OK )
				{
					String str = "Unable to set texture in D3D9";
					CM_EXCEPT(RenderingAPIException, str);
				}

				// set stage desc.
				mTexStageDesc[unit].pTex = pTex;
				mTexStageDesc[unit].texType = D3D9Mappings::get(dt->getTextureType());

				// Set gamma now too
				if (dt->isHardwareGammaReadToBeUsed())
				{
					__SetSamplerState(static_cast<DWORD>(unit), D3DSAMP_SRGBTEXTURE, TRUE);
				}
				else
				{
					__SetSamplerState(static_cast<DWORD>(unit), D3DSAMP_SRGBTEXTURE, FALSE);
				}

				mRenderStats.numTextureBinds++;
				mRenderStats.numSamplerBinds++;
			}
		}
		else
		{
			if (mTexStageDesc[unit].pTex != 0)
			{
				hr = getActiveD3D9Device()->SetTexture(static_cast<DWORD>(unit), 0);
				if( hr != S_OK )
				{
					String str = "Unable to disable texture '" + toString(unit) + "' in D3D9";
					CM_EXCEPT(RenderingAPIException, str);
				}

				mRenderStats.numTextureBinds++;
			}

			hr = __SetTextureStageState(static_cast<DWORD>(unit), D3DTSS_COLOROP, D3DTOP_DISABLE);
			if( hr != S_OK )
			{
				String str = "Unable to disable texture '" + toString(unit) + "' in D3D9";
				CM_EXCEPT(RenderingAPIException, str);
			}

			// set stage desc. to defaults
			mTexStageDesc[unit].pTex = 0;
			mTexStageDesc[unit].coordIndex = 0;
			mTexStageDesc[unit].texType = D3D9Mappings::D3D_TEX_TYPE_NORMAL;
		}
	}

	void D3D9RenderSystem::setSamplerState(GpuProgramType gptype, UINT16 unit, const SamplerStatePtr& state)
	{
		THROW_IF_NOT_CORE_THREAD;

		if(gptype != GPT_FRAGMENT_PROGRAM && gptype != GPT_VERTEX_PROGRAM)
		{
			LOGWRN("D3D9 doesn't support this gpu program type: " + toString(gptype));
			return;
		}

		if(gptype == GPT_VERTEX_PROGRAM)
		{
			unit = D3DVERTEXTEXTURESAMPLER0 + unit; // Vertex stage uses special samplers
		}

		// Set texture layer filtering
		setTextureFiltering(unit, FT_MIN, state->getTextureFiltering(FT_MIN));
		setTextureFiltering(unit, FT_MAG, state->getTextureFiltering(FT_MAG));
		setTextureFiltering(unit, FT_MIP, state->getTextureFiltering(FT_MIP));

		// Set texture layer filtering
		if (state->getTextureAnisotropy() > 0)
			setTextureAnisotropy(unit, state->getTextureAnisotropy());

		// Set mipmap biasing
		setTextureMipmapBias(unit, state->getTextureMipmapBias());

		// Texture addressing mode
		const UVWAddressingMode& uvw = state->getTextureAddressingMode();
		setTextureAddressingMode(unit, uvw);

		// Set border color
		setTextureBorderColor(unit, state->getBorderColor());

		mRenderStats.numSamplerBinds++;
	}

	void D3D9RenderSystem::setBlendState(const BlendStatePtr& blendState)
	{
		THROW_IF_NOT_CORE_THREAD;

		// Alpha to coverage
		setAlphaToCoverage(blendState->getAlphaToCoverageEnabled());

		// Blend states
		// DirectX 9 doesn't allow us to specify blend state per render target, so we just use the first one.
		if(blendState->getBlendEnabled(0))
		{
			setSceneBlending(blendState->getSrcBlend(0), blendState->getDstBlend(0), blendState->getAlphaSrcBlend(0), blendState->getAlphaDstBlend(0)
				, blendState->getBlendOperation(0), blendState->getAlphaBlendOperation(0));
		}
		else
		{
			setSceneBlending(BF_ONE, BF_ZERO, BO_ADD);
		}

		// Color write mask
		UINT8 writeMask = blendState->getRenderTargetWriteMask(0);
		setColorBufferWriteEnabled((writeMask & 0x1) != 0, (writeMask & 0x2) != 0, (writeMask & 0x4) != 0, (writeMask & 0x8) != 0);

		mRenderStats.numBlendStateChanges++;
	}

	void D3D9RenderSystem::setRasterizerState(const RasterizerStatePtr& rasterizerState)
	{
		THROW_IF_NOT_CORE_THREAD;

		setDepthBias((float)rasterizerState->getDepthBias(), rasterizerState->getSlopeScaledDepthBias());

		setCullingMode(rasterizerState->getCullMode());

		setPolygonMode(rasterizerState->getPolygonMode());

		setScissorTestEnable(rasterizerState->getScissorEnable());

		setMultisampleAntialiasEnable(rasterizerState->getMultisampleEnable());

		setAntialiasedLineEnable(rasterizerState->getAntialiasedLineEnable());

		mRenderStats.numRasterizerStateChanges++;
	}

	void D3D9RenderSystem::setDepthStencilState(const DepthStencilStatePtr& depthStencilState, UINT32 stencilRefValue)
	{
		THROW_IF_NOT_CORE_THREAD;

		// Set stencil buffer options
		setStencilCheckEnabled(depthStencilState->getStencilEnable());

		setStencilBufferOperations(depthStencilState->getStencilFrontFailOp(), depthStencilState->getStencilFrontZFailOp(), depthStencilState->getStencilFrontPassOp(), true);
		setStencilBufferFunc(depthStencilState->getStencilFrontCompFunc(), true);

		setStencilBufferOperations(depthStencilState->getStencilBackFailOp(), depthStencilState->getStencilBackZFailOp(), depthStencilState->getStencilBackPassOp(), false);
		setStencilBufferFunc(depthStencilState->getStencilBackCompFunc(), false);

		setStencilBufferReadMask(depthStencilState->getStencilReadMask());
		setStencilBufferWriteMask(depthStencilState->getStencilWriteMask());

		// Set depth buffer options
		setDepthBufferCheckEnabled(depthStencilState->getDepthReadEnable());
		setDepthBufferWriteEnabled(depthStencilState->getDepthWriteEnable());
		setDepthBufferFunction(depthStencilState->getDepthComparisonFunc());		

		// Set stencil ref value
		setStencilRefValue(stencilRefValue);

		mRenderStats.numDepthStencilStateChanges++;
	}

	void D3D9RenderSystem::setTextureMipmapBias(UINT16 unit, float bias)
	{
		THROW_IF_NOT_CORE_THREAD;

		if (mCurrentCapabilities->hasCapability(RSC_MIPMAP_LOD_BIAS))
		{
			// ugh - have to pass float data through DWORD with no conversion
			HRESULT hr = __SetSamplerState(static_cast<DWORD>(unit), D3DSAMP_MIPMAPLODBIAS, 
				*(DWORD*)&bias);
			if(FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Unable to set texture mipmap bias");
		}
	}

	void D3D9RenderSystem::setTextureAddressingMode( UINT16 stage, 
		const UVWAddressingMode& uvw )
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if( FAILED( hr = __SetSamplerState( static_cast<DWORD>(stage), D3DSAMP_ADDRESSU, D3D9Mappings::get(uvw.u, mDeviceManager->getActiveDevice()->getD3D9DeviceCaps()) ) ) )
			CM_EXCEPT(RenderingAPIException, "Failed to set texture addressing mode for U" );
		if( FAILED( hr = __SetSamplerState( static_cast<DWORD>(stage), D3DSAMP_ADDRESSV, D3D9Mappings::get(uvw.v, mDeviceManager->getActiveDevice()->getD3D9DeviceCaps()) ) ) )
			CM_EXCEPT(RenderingAPIException, "Failed to set texture addressing mode for V");
		if( FAILED( hr = __SetSamplerState( static_cast<DWORD>(stage), D3DSAMP_ADDRESSW, D3D9Mappings::get(uvw.w, mDeviceManager->getActiveDevice()->getD3D9DeviceCaps()) ) ) )
			CM_EXCEPT(RenderingAPIException, "Failed to set texture addressing mode for W");
	}

	void D3D9RenderSystem::setTextureBorderColor(UINT16 stage, const Color& colour)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if( FAILED( hr = __SetSamplerState( static_cast<DWORD>(stage), D3DSAMP_BORDERCOLOR, colour.getAsBGRA()) ) )
			CM_EXCEPT(RenderingAPIException, "Failed to set texture border colour");
	}

	void D3D9RenderSystem::setSceneBlending( BlendFactor sourceFactor, BlendFactor destFactor, BlendOperation op )
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if( sourceFactor == BF_ONE && destFactor == BF_ZERO)
		{
			if (FAILED(hr = __SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE)))
				CM_EXCEPT(RenderingAPIException, "Failed to set alpha blending option");
		}
		else
		{
			if (FAILED(hr = __SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)))
				CM_EXCEPT(RenderingAPIException, "Failed to set alpha blending option");
			if (FAILED(hr = __SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE)))
				CM_EXCEPT(RenderingAPIException, "Failed to set separate alpha blending option");
			if( FAILED( hr = __SetRenderState( D3DRS_SRCBLEND, D3D9Mappings::get(sourceFactor) ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to set source blend");
			if( FAILED( hr = __SetRenderState( D3DRS_DESTBLEND, D3D9Mappings::get(destFactor) ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to set destination blend");
		}

		if (FAILED(hr = __SetRenderState(D3DRS_BLENDOP, D3D9Mappings::get(op))))
			CM_EXCEPT(RenderingAPIException, "Failed to set scene blending operation option");
		if (FAILED(hr = __SetRenderState(D3DRS_BLENDOPALPHA, D3D9Mappings::get(op))))
			CM_EXCEPT(RenderingAPIException, "Failed to set scene blending operation option");
	}

	void D3D9RenderSystem::setSceneBlending( BlendFactor sourceFactor, BlendFactor destFactor, BlendFactor sourceFactorAlpha, 
		BlendFactor destFactorAlpha, BlendOperation op, BlendOperation alphaOp )
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if( sourceFactor == BF_ONE && destFactor == BF_ZERO && 
			sourceFactorAlpha == BF_ONE && destFactorAlpha == BF_ZERO)
		{
			if (FAILED(hr = __SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE)))
				CM_EXCEPT(RenderingAPIException, "Failed to set alpha blending option");
		}
		else
		{
			if (FAILED(hr = __SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)))
				CM_EXCEPT(RenderingAPIException, "Failed to set alpha blending option");
			if (FAILED(hr = __SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE)))
				CM_EXCEPT(RenderingAPIException, "Failed to set separate alpha blending option");
			if( FAILED( hr = __SetRenderState( D3DRS_SRCBLEND, D3D9Mappings::get(sourceFactor) ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to set source blend");
			if( FAILED( hr = __SetRenderState( D3DRS_DESTBLEND, D3D9Mappings::get(destFactor) ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to set destination blend");
			if( FAILED( hr = __SetRenderState( D3DRS_SRCBLENDALPHA, D3D9Mappings::get(sourceFactorAlpha) ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to set alpha source blend");
			if( FAILED( hr = __SetRenderState( D3DRS_DESTBLENDALPHA, D3D9Mappings::get(destFactorAlpha) ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to set alpha destination blend");
		}

		if (FAILED(hr = __SetRenderState(D3DRS_BLENDOP, D3D9Mappings::get(op))))
			CM_EXCEPT(RenderingAPIException, "Failed to set scene blending operation option");
		if (FAILED(hr = __SetRenderState(D3DRS_BLENDOPALPHA, D3D9Mappings::get(alphaOp))))
			CM_EXCEPT(RenderingAPIException, "Failed to set alpha scene blending operation option");
	}

	void D3D9RenderSystem::setAlphaTest(CompareFunction func, unsigned char value)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;

		if (func != CMPF_ALWAYS_PASS)
		{
			if( FAILED( hr = __SetRenderState( D3DRS_ALPHATESTENABLE,  TRUE ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to enable alpha testing");
		}
		else
		{
			if( FAILED( hr = __SetRenderState( D3DRS_ALPHATESTENABLE,  FALSE ) ) )
				CM_EXCEPT(RenderingAPIException, "Failed to disable alpha testing");
		}
		// Set always just be sure
		if( FAILED( hr = __SetRenderState( D3DRS_ALPHAFUNC, D3D9Mappings::get(func) ) ) )
			CM_EXCEPT(RenderingAPIException, "Failed to set alpha reject function");
		if( FAILED( hr = __SetRenderState( D3DRS_ALPHAREF, value ) ) )
			CM_EXCEPT(RenderingAPIException, "Failed to set render state D3DRS_ALPHAREF");
	}

	void D3D9RenderSystem::setAlphaToCoverage(bool enable)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		static bool lasta2c = false;

		// Alpha to coverage
		if (getCapabilities()->hasCapability(RSC_ALPHA_TO_COVERAGE))
		{
			// Vendor-specific hacks on renderstate, gotta love 'em
			if (getCapabilities()->getVendor() == GPU_NVIDIA)
			{
				if (enable)
				{
					if( FAILED( hr = __SetRenderState( D3DRS_ADAPTIVETESS_Y,  (D3DFORMAT)MAKEFOURCC('A', 'T', 'O', 'C') ) ) )
						CM_EXCEPT(RenderingAPIException, "Failed to set alpha to coverage option");
				}
				else
				{
					if( FAILED( hr = __SetRenderState( D3DRS_ADAPTIVETESS_Y,  D3DFMT_UNKNOWN ) ) )
						CM_EXCEPT(RenderingAPIException, "Failed to set alpha to coverage option");
				}

			}
			else if ((getCapabilities()->getVendor() == GPU_AMD))
			{
				if (enable)
				{
					if( FAILED( hr = __SetRenderState( D3DRS_POINTSIZE,  MAKEFOURCC('A','2','M','1') ) ) )
						CM_EXCEPT(RenderingAPIException, "Failed to set alpha to coverage option");
				}
				else
				{
					// discovered this through trial and error, seems to work
					if( FAILED( hr = __SetRenderState( D3DRS_POINTSIZE,  MAKEFOURCC('A','2','M','0') ) ) )
						CM_EXCEPT(RenderingAPIException, "Failed to set alpha to coverage option");
				}
			}

			lasta2c = enable;
		}
	}

	void D3D9RenderSystem::setCullingMode(CullingMode mode)
	{
		THROW_IF_NOT_CORE_THREAD;

		mCullingMode = mode;
		HRESULT hr;

		if( FAILED (hr = __SetRenderState(D3DRS_CULLMODE, 
			D3D9Mappings::get(mode, false))) )
			CM_EXCEPT(RenderingAPIException, "Failed to set culling mode");
	}

	void D3D9RenderSystem::setDepthBufferParams(bool depthTest, bool depthWrite, CompareFunction depthFunction)
	{
		THROW_IF_NOT_CORE_THREAD;

		setDepthBufferCheckEnabled( depthTest );
		setDepthBufferWriteEnabled( depthWrite );
		setDepthBufferFunction( depthFunction );
	}

	void D3D9RenderSystem::setDepthBufferCheckEnabled(bool enabled)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;

		if( enabled )
			hr = __SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		else
			hr = __SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

		if(FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error setting depth buffer test state");
	}

	void D3D9RenderSystem::setDepthBufferWriteEnabled(bool enabled)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;

		if( FAILED( hr = __SetRenderState( D3DRS_ZWRITEENABLE, enabled ) ) )
			CM_EXCEPT(RenderingAPIException, "Error setting depth buffer write state");
	}

	void D3D9RenderSystem::setDepthBufferFunction(CompareFunction func)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if( FAILED( hr = __SetRenderState( D3DRS_ZFUNC, D3D9Mappings::get(func) ) ) )
			CM_EXCEPT(RenderingAPIException, "Error setting depth buffer test function");
	}

	void D3D9RenderSystem::setDepthBias(float constantBias, float slopeScaleBias)
	{
		THROW_IF_NOT_CORE_THREAD;

		if ((mDeviceManager->getActiveDevice()->getD3D9DeviceCaps().RasterCaps & D3DPRASTERCAPS_DEPTHBIAS) != 0)
		{
			// Negate bias since D3D is backward
			// D3D also expresses the constant bias as an absolute value, rather than 
			// relative to minimum depth unit, so scale to fit
			constantBias = -constantBias / 250000.0f;
			HRESULT hr = __SetRenderState(D3DRS_DEPTHBIAS, FLOAT2DWORD(constantBias));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting constant depth bias");
		}

		if ((mDeviceManager->getActiveDevice()->getD3D9DeviceCaps().RasterCaps & D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS) != 0)
		{
			// Negate bias since D3D is backward
			slopeScaleBias = -slopeScaleBias;
			HRESULT hr = __SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, FLOAT2DWORD(slopeScaleBias));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting slope scale depth bias");
		}


	}

	void D3D9RenderSystem::setColorBufferWriteEnabled(bool red, bool green, bool blue, bool alpha)
	{
		THROW_IF_NOT_CORE_THREAD;

		DWORD val = 0;
		if (red) 
			val |= D3DCOLORWRITEENABLE_RED;
		if (green)
			val |= D3DCOLORWRITEENABLE_GREEN;
		if (blue)
			val |= D3DCOLORWRITEENABLE_BLUE;
		if (alpha)
			val |= D3DCOLORWRITEENABLE_ALPHA;

		HRESULT hr = __SetRenderState(D3DRS_COLORWRITEENABLE, val); 
		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error setting colour write enable flags");
	}

	void D3D9RenderSystem::setPolygonMode(PolygonMode level)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr = __SetRenderState(D3DRS_FILLMODE, D3D9Mappings::get(level));
		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error setting polygon mode.");
	}

	void D3D9RenderSystem::setStencilCheckEnabled(bool enabled)
	{
		THROW_IF_NOT_CORE_THREAD;

		// Allow stencilling
		HRESULT hr = __SetRenderState(D3DRS_STENCILENABLE, enabled);
		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error enabling / disabling stencilling.");

		if (mCurrentCapabilities->hasCapability(RSC_TWO_SIDED_STENCIL))
		{
			hr = __SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, TRUE);

			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting 2-sided stencil mode.");
		}
		else
		{
			hr = __SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);

			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting 1-sided stencil mode.");
		}
	}

	void D3D9RenderSystem::setStencilBufferOperations(StencilOperation stencilFailOp, StencilOperation depthFailOp, StencilOperation passOp, bool ccw)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;

		// 2-sided operation
		if (ccw)
		{
			// fail op
			hr = __SetRenderState(D3DRS_CCW_STENCILFAIL, D3D9Mappings::get(stencilFailOp));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting stencil fail operation (ccw).");

			// depth fail op
			hr = __SetRenderState(D3DRS_CCW_STENCILZFAIL, D3D9Mappings::get(depthFailOp));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting stencil depth fail operation (ccw).");

			// pass op
			hr = __SetRenderState(D3DRS_CCW_STENCILPASS, D3D9Mappings::get(passOp));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting stencil pass operation (ccw).");
		}
		else
		{
			// fail op
			hr = __SetRenderState(D3DRS_STENCILFAIL, D3D9Mappings::get(stencilFailOp, true));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting stencil fail operation (cw).");

			// depth fail op
			hr = __SetRenderState(D3DRS_STENCILZFAIL, D3D9Mappings::get(depthFailOp, true));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting stencil depth fail operation (cw).");

			// pass op
			hr = __SetRenderState(D3DRS_STENCILPASS, D3D9Mappings::get(passOp, true));
			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Error setting stencil pass operation (cw).");
		}
	}

	void D3D9RenderSystem::setStencilBufferFunc(CompareFunction func, bool ccw)
	{
		HRESULT hr;
		
		if(ccw)
			hr = __SetRenderState(D3DRS_CCW_STENCILFUNC, D3D9Mappings::get(func));
		else
			hr = __SetRenderState(D3DRS_STENCILFUNC, D3D9Mappings::get(func));

		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error setting stencil buffer test function.");
	}

	void D3D9RenderSystem::setStencilBufferReadMask(UINT32 mask)
	{
		HRESULT hr = __SetRenderState(D3DRS_STENCILMASK, mask);

		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error setting stencil buffer mask.");
	}

	void D3D9RenderSystem::setStencilBufferWriteMask(UINT32 mask)
	{
		HRESULT hr = __SetRenderState(D3DRS_STENCILWRITEMASK, mask);

		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error setting stencil buffer write mask.");
	}

	void D3D9RenderSystem::setStencilRefValue(UINT32 refValue)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr = __SetRenderState(D3DRS_STENCILREF, refValue);
		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Error setting stencil buffer reference value.");
	}

	void D3D9RenderSystem::setTextureFiltering(UINT16 unit, FilterType ftype, FilterOptions filter)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		D3D9Mappings::eD3DTexType texType = mTexStageDesc[unit].texType;
		hr = __SetSamplerState( static_cast<DWORD>(unit), D3D9Mappings::get(ftype), 
			D3D9Mappings::get(ftype, filter, mDeviceManager->getActiveDevice()->getD3D9DeviceCaps(), texType));

		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Failed to set texture filter ");
	}


	void D3D9RenderSystem::setTextureAnisotropy(UINT16 unit, unsigned int maxAnisotropy)
	{
		THROW_IF_NOT_CORE_THREAD;

		if (static_cast<DWORD>(maxAnisotropy) > mDeviceManager->getActiveDevice()->getD3D9DeviceCaps().MaxAnisotropy)
			maxAnisotropy = mDeviceManager->getActiveDevice()->getD3D9DeviceCaps().MaxAnisotropy;

		if (_getCurrentAnisotropy(unit) != maxAnisotropy)
			__SetSamplerState( static_cast<DWORD>(unit), D3DSAMP_MAXANISOTROPY, maxAnisotropy );
	}

	void D3D9RenderSystem::setRenderTarget(RenderTargetPtr target)
	{
		THROW_IF_NOT_CORE_THREAD;

		mActiveRenderTarget = target;

		HRESULT hr;

		// Possibly change device if the target is a window
		if (target->isWindow())
		{
			D3D9RenderWindow* window = static_cast<D3D9RenderWindow*>(target.get());
			mDeviceManager->setActiveRenderTargetDevice(window->_getDevice());
			window->_validateDevice();
		}

		// Retrieve render surfaces
		UINT32 maxRenderTargets = mCurrentCapabilities->getNumMultiRenderTargets();
		IDirect3DSurface9** pBack = cm_newN<IDirect3DSurface9*, ScratchAlloc>(maxRenderTargets);
		memset(pBack, 0, sizeof(IDirect3DSurface9*) * maxRenderTargets);
		target->getCustomAttribute( "DDBACKBUFFER", pBack );
		if (!pBack[0])
		{
			cm_deleteN<ScratchAlloc>(pBack, maxRenderTargets);
			return;
		}

		IDirect3DSurface9* pDepth = NULL;

		if (!pDepth)
			target->getCustomAttribute( "D3DZBUFFER", &pDepth );
		
		// Bind render targets
		for(UINT32 x = 0; x < maxRenderTargets; ++x)
		{
			hr = getActiveD3D9Device()->SetRenderTarget(x, pBack[x]);
			if (FAILED(hr))
			{
				String msg = DXGetErrorDescription(hr);
				CM_EXCEPT(RenderingAPIException, "Failed to setRenderTarget : " + msg);
			}
		}

		cm_deleteN<ScratchAlloc>(pBack, maxRenderTargets);

		hr = getActiveD3D9Device()->SetDepthStencilSurface(pDepth);
		if (FAILED(hr))
		{
			String msg = DXGetErrorDescription(hr);
			CM_EXCEPT(RenderingAPIException, "Failed to setDepthStencil : " + msg);
		}

		mRenderStats.numRenderTargetChanges++;
	}

	void D3D9RenderSystem::setViewport(const ViewportPtr& vp)
	{
		THROW_IF_NOT_CORE_THREAD;

		assert(vp != nullptr);

		// ok, it's different, time to set render target and viewport params
		D3DVIEWPORT9 d3dvp;
		HRESULT hr;

		// Set render target
		RenderTargetPtr target = vp->getTarget();
		setRenderTarget(target);

		setCullingMode( mCullingMode );

		// set viewport dimensions
		mViewportWidth = vp->getWidth();
		mViewportHeight = vp->getHeight();
		mViewportLeft = vp->getX();
		mViewportTop = vp->getY();

		d3dvp.X = vp->getX();
		d3dvp.Y = vp->getY();
		d3dvp.Width = vp->getWidth();
		d3dvp.Height = vp->getHeight();
		if (target->requiresTextureFlipping())
		{
			// Convert "top-left" to "bottom-left"
			d3dvp.Y = target->getHeight() - d3dvp.Height - d3dvp.Y;
		}

		// Z-values from 0.0 to 1.0 (TODO: standardise with OpenGL)
		d3dvp.MinZ = 0.0f;
		d3dvp.MaxZ = 1.0f;

		if( FAILED( hr = getActiveD3D9Device()->SetViewport( &d3dvp ) ) )
			CM_EXCEPT(RenderingAPIException, "Failed to set viewport.");

		// Set sRGB write mode
		__SetRenderState(D3DRS_SRGBWRITEENABLE, target->isHwGammaEnabled());
	}

	void D3D9RenderSystem::beginFrame()
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if(FAILED(hr = getActiveD3D9Device()->BeginScene()))
		{
			String msg = DXGetErrorDescription(hr);
			CM_EXCEPT(RenderingAPIException, "Error beginning frame :" + msg);
		}

 		mDeviceManager->getActiveDevice()->clearDeviceStreams();
		mIsFrameInProgress = true;
	}

	void D3D9RenderSystem::endFrame()
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if(FAILED(hr = getActiveD3D9Device()->EndScene()))
			CM_EXCEPT(RenderingAPIException, "Error ending frame");

		mIsFrameInProgress = false;
	}

	void D3D9RenderSystem::setVertexDeclaration(VertexDeclarationPtr decl)
	{
		THROW_IF_NOT_CORE_THREAD;

		std::shared_ptr<D3D9VertexDeclaration> d3ddecl = std::static_pointer_cast<D3D9VertexDeclaration>(decl);

		HRESULT hr;
		if (FAILED(hr = getActiveD3D9Device()->SetVertexDeclaration(d3ddecl->getD3DVertexDeclaration())))
		{
			CM_EXCEPT(RenderingAPIException, "Unable to set D3D9 vertex declaration");
		}
	}

	void D3D9RenderSystem::setVertexBuffers(UINT32 index, VertexBufferPtr* buffers, UINT32 numBuffers)
	{
		THROW_IF_NOT_CORE_THREAD;

		UINT32 maxBoundVertexBuffers = mCurrentCapabilities->getMaxBoundVertexBuffers();
		if(index < 0 || (index + numBuffers) > maxBoundVertexBuffers)
			CM_EXCEPT(InvalidParametersException, "Invalid vertex index: " + toString(index) + ". Valid range is 0 .. " + toString(maxBoundVertexBuffers - 1));

		HRESULT hr;

		for(UINT32 i = 0; i < numBuffers; i++)
		{
			if(buffers[i] != nullptr)
			{
				D3D9VertexBuffer* d3d9buf = static_cast<D3D9VertexBuffer*>(buffers[i].get());

				hr = getActiveD3D9Device()->SetStreamSource(
					static_cast<UINT>(index + i),
					d3d9buf->getD3D9VertexBuffer(),
					0,
					static_cast<UINT>(d3d9buf->getVertexSize()) // stride
					);
			}
			else
			{
				hr = getActiveD3D9Device()->SetStreamSource(static_cast<UINT>(index + i), nullptr, 0, 0);
			}

			if (FAILED(hr))
				CM_EXCEPT(RenderingAPIException, "Unable to set D3D9 stream source for buffer binding");

			mRenderStats.numVertexBufferBinds++;
		}
	}

	void D3D9RenderSystem::setIndexBuffer(const IndexBufferPtr& buffer)
	{
		THROW_IF_NOT_CORE_THREAD;

		D3D9IndexBuffer* d3dIdxBuf = static_cast<D3D9IndexBuffer*>(buffer.get());

		HRESULT hr = getActiveD3D9Device()->SetIndices( d3dIdxBuf->getD3DIndexBuffer() );
		if (FAILED(hr))
			CM_EXCEPT(RenderingAPIException, "Failed to set index buffer");

		mRenderStats.numIndexBufferBinds++;
	}

	void D3D9RenderSystem::setDrawOperation(DrawOperationType op)
	{
		THROW_IF_NOT_CORE_THREAD;

		mCurrentDrawOperation = op;
	}

	void D3D9RenderSystem::draw(UINT32 vertexOffset, UINT32 vertexCount)
	{
		UINT32 primCount = vertexCountToPrimCount(mCurrentDrawOperation, vertexCount);

		HRESULT hr = getActiveD3D9Device()->DrawPrimitive(getD3D9PrimitiveType(), static_cast<UINT>(vertexOffset), static_cast<UINT>(primCount)); 

		if(FAILED(hr))
		{
			String msg = DXGetErrorDescription(hr);
			CM_EXCEPT(RenderingAPIException, "Failed to DrawPrimitive : " + msg);
		}

		mRenderStats.numDrawCalls++;
		mRenderStats.numVertices += vertexCount;
		mRenderStats.numPrimitives += primCount;
	}

	void D3D9RenderSystem::drawIndexed(UINT32 startIndex, UINT32 indexCount, UINT32 vertexOffset, UINT32 vertexCount)
	{
		UINT32 primCount = vertexCountToPrimCount(mCurrentDrawOperation, indexCount);

		// do indexed draw operation
		HRESULT hr = getActiveD3D9Device()->DrawIndexedPrimitive(
			getD3D9PrimitiveType(), 
			static_cast<UINT>(vertexOffset), 
			0, 
			static_cast<UINT>(vertexCount), 
			static_cast<UINT>(startIndex), 
			static_cast<UINT>(primCount)
			);

		if(FAILED(hr))
		{
			String msg = DXGetErrorDescription(hr);
			CM_EXCEPT(RenderingAPIException, "Failed to DrawIndexedPrimitive : " + msg);
		}

		mRenderStats.numDrawCalls++;
		mRenderStats.numVertices += vertexCount;
		mRenderStats.numPrimitives += primCount;
	}

	void D3D9RenderSystem::setScissorRect(UINT32 left, UINT32 top, UINT32 right, UINT32 bottom)
	{
		THROW_IF_NOT_CORE_THREAD;

		mScissorRect.left = static_cast<LONG>(left);
		mScissorRect.top = static_cast<LONG>(top);
		mScissorRect.bottom = static_cast<LONG>(bottom);
		mScissorRect.right = static_cast<LONG>(right);
	}

	void D3D9RenderSystem::setScissorTestEnable(bool enable)
	{
		THROW_IF_NOT_CORE_THREAD;

		HRESULT hr;
		if (enable)
		{
			if (FAILED(hr = __SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE)))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to enable scissor rendering state; " + getErrorDescription(hr));
			}

			if (FAILED(hr = getActiveD3D9Device()->SetScissorRect(&mScissorRect)))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to set scissor rectangle; " + getErrorDescription(hr));
			}
		}
		else
		{
			if (FAILED(hr = __SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE)))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to disable scissor rendering state; " + getErrorDescription(hr));
			}
		}
	}

	void D3D9RenderSystem::setMultisampleAntialiasEnable(bool enable)
	{
		HRESULT hr;
		if(enable)
		{
			if (FAILED(hr = __SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE)))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to enable multisample antialiasing. Error description: " + getErrorDescription(hr));
			}
		}
		else
		{
			if (FAILED(hr = __SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE)))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to disable multisample antialiasing. Error description: " + getErrorDescription(hr));
			}
		}
	}

	void D3D9RenderSystem::setAntialiasedLineEnable(bool enable)
	{
		HRESULT hr;
		if(enable)
		{
			if (FAILED(hr = __SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE)))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to enable line antialiasing. Error description: " + getErrorDescription(hr));
			}
		}
		else
		{
			if (FAILED(hr = __SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE)))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to disable line antialiasing. Error description: " + getErrorDescription(hr));
			}
		}
	}

	void D3D9RenderSystem::clearRenderTarget(UINT32 buffers, const Color& color, float depth, UINT16 stencil)
	{
		if(mActiveRenderTarget == nullptr)
			return;

		RectI clearRect(0, 0, mActiveRenderTarget->getWidth(), mActiveRenderTarget->getHeight());

		clearArea(buffers, color, depth, stencil, clearRect);
	}

	void D3D9RenderSystem::clearViewport(UINT32 buffers, const Color& color, float depth, UINT16 stencil)
	{
		RectI clearRect(mViewportLeft, mViewportTop, mViewportWidth, mViewportHeight);

		clearArea(buffers, color, depth, stencil, clearRect);
	}

	void D3D9RenderSystem::clearArea(UINT32 buffers, const Color& color, float depth, UINT16 stencil, const RectI& clearRect)
	{
		THROW_IF_NOT_CORE_THREAD;

		if(mActiveRenderTarget == nullptr)
			return;

		DWORD flags = 0;
		if (buffers & FBT_COLOR)
		{
			flags |= D3DCLEAR_TARGET;
		}
		if (buffers & FBT_DEPTH)
		{
			flags |= D3DCLEAR_ZBUFFER;
		}

		// Only try to clear the stencil buffer if supported
		if (buffers & FBT_STENCIL)
		{
			flags |= D3DCLEAR_STENCIL;
		}

		bool clearEntireTarget = clearRect.width == 0 || clearRect.height == 0;
		clearEntireTarget |= (clearRect.x == 0 && clearRect.y == 0 && clearRect.width == mActiveRenderTarget->getWidth() && clearRect.height == mActiveRenderTarget->getHeight());

		if(!clearEntireTarget)
		{
			D3DRECT clearD3DRect;
			clearD3DRect.x1 = (LONG)Math::clamp(clearRect.x, 0, (INT32)mActiveRenderTarget->getWidth() - 1);
			clearD3DRect.x2 = (LONG)Math::clamp((INT32)clearD3DRect.x1 + clearRect.width, 0, (INT32)mActiveRenderTarget->getWidth() - 1);

			clearD3DRect.y1 = (LONG)Math::clamp(clearRect.y, 0, (INT32)mActiveRenderTarget->getHeight() - 1);
			clearD3DRect.y2 = (LONG)Math::clamp((INT32)clearD3DRect.y1 + clearRect.height, 0, (INT32)mActiveRenderTarget->getHeight() - 1);

			HRESULT hr;
			if(FAILED(hr = getActiveD3D9Device()->Clear(1, &clearD3DRect, flags, color.getAsBGRA(), depth, stencil)))
			{
				String msg = DXGetErrorDescription(hr);
				CM_EXCEPT(RenderingAPIException, "Error clearing frame buffer : " + msg);
			}
		}
		else
		{
			HRESULT hr;
			if(FAILED(hr = getActiveD3D9Device()->Clear(0, nullptr, flags, color.getAsBGRA(), depth, stencil)))
			{
				String msg = DXGetErrorDescription(hr);
				CM_EXCEPT(RenderingAPIException, "Error clearing frame buffer : " + msg);
			}
		}

		mRenderStats.numClears++;
	}

	IDirect3D9*	D3D9RenderSystem::getDirect3D9()
	{
		THROW_IF_NOT_CORE_THREAD;

		IDirect3D9* pDirect3D9 = msD3D9RenderSystem->mpD3D;

		if (pDirect3D9 == NULL)
		{
			CM_EXCEPT(InvalidParametersException, "Direct3D9 interface is NULL !!!");
		}

		return pDirect3D9;
	}

	UINT D3D9RenderSystem::getResourceCreationDeviceCount()
	{
		THROW_IF_NOT_CORE_THREAD;

		D3D9ResourceCreationPolicy creationPolicy = msD3D9RenderSystem->mResourceManager->getCreationPolicy();

		if (creationPolicy == RCP_CREATE_ON_ACTIVE_DEVICE)
		{
			return 1;
		}
		else if (creationPolicy == RCP_CREATE_ON_ALL_DEVICES)
		{
			return msD3D9RenderSystem->mDeviceManager->getDeviceCount();
		}

		CM_EXCEPT(InvalidParametersException, "Invalid resource creation policy !!!" );

		return 0;
	}

	IDirect3DDevice9* D3D9RenderSystem::getResourceCreationDevice(UINT index)
	{
		THROW_IF_NOT_CORE_THREAD;

		D3D9ResourceCreationPolicy creationPolicy = msD3D9RenderSystem->mResourceManager->getCreationPolicy();
		IDirect3DDevice9* d3d9Device = NULL;

		if (creationPolicy == RCP_CREATE_ON_ACTIVE_DEVICE)
		{
			d3d9Device = msD3D9RenderSystem->getActiveD3D9Device();
		}
		else if (creationPolicy == RCP_CREATE_ON_ALL_DEVICES) 
		{
			d3d9Device = msD3D9RenderSystem->mDeviceManager->getDevice(index)->getD3D9Device();
		}
		else
		{
			CM_EXCEPT(InvalidParametersException, "Invalid resource creation policy !!!" );
		}

		return d3d9Device;
	}

	IDirect3DDevice9* D3D9RenderSystem::getActiveD3D9Device()
	{	
		THROW_IF_NOT_CORE_THREAD;

		D3D9Device* activeDevice = msD3D9RenderSystem->mDeviceManager->getActiveDevice();
		IDirect3DDevice9* d3d9Device;

		d3d9Device = activeDevice->getD3D9Device();

		if (d3d9Device == NULL)
		{
			CM_EXCEPT(InvalidParametersException, "Current d3d9 device is NULL !!!" );
		}

		return d3d9Device;
	}	

	D3D9ResourceManager* D3D9RenderSystem::getResourceManager()
	{
		// No need to check if we're on core thread as this is synced up internally

		return msD3D9RenderSystem->mResourceManager;
	}

	D3D9DeviceManager* D3D9RenderSystem::getDeviceManager()
	{
		THROW_IF_NOT_CORE_THREAD;

		return msD3D9RenderSystem->mDeviceManager;
	}

	/************************************************************************/
	/* 								UTILITY METHODS                    		*/
	/************************************************************************/

	float D3D9RenderSystem::getHorizontalTexelOffset()
	{
		// D3D considers the origin to be in the center of a pixel
		return -0.5f;
	}

	float D3D9RenderSystem::getVerticalTexelOffset()
	{
		// D3D considers the origin to be in the center of a pixel
		return -0.5f;
	}

	float D3D9RenderSystem::getMinimumDepthInputValue()
	{
		// Range [0.0f, 1.0f]
		return 0.0f;
	}

	float D3D9RenderSystem::getMaximumDepthInputValue()
	{
		// Range [0.0f, 1.0f]
		// D3D inverts even identity view matrices, so maximum INPUT is -1.0
		return -1.0f;
	}

	VertexElementType D3D9RenderSystem::getColorVertexElementType() const
	{
		return VET_COLOR_ARGB;
	}

	void D3D9RenderSystem::convertProjectionMatrix(const Matrix4& matrix, Matrix4& dest, bool forGpuProgram)
	{
		dest = matrix;

		// Convert depth range from [-1,+1] to [0,1]
		dest[2][0] = (dest[2][0] + dest[3][0]) / 2;
		dest[2][1] = (dest[2][1] + dest[3][1]) / 2;
		dest[2][2] = (dest[2][2] + dest[3][2]) / 2;
		dest[2][3] = (dest[2][3] + dest[3][3]) / 2;

		if (!forGpuProgram)
		{
			// Convert right-handed to left-handed
			dest[0][2] = -dest[0][2];
			dest[1][2] = -dest[1][2];
			dest[2][2] = -dest[2][2];
			dest[3][2] = -dest[3][2];
		}
	}

	/************************************************************************/
	/* 								PRIVATE		                     		*/
	/************************************************************************/

	D3D9DriverList* D3D9RenderSystem::getDirect3DDrivers() const
	{
		if( !mDriverList )
			mDriverList = cm_new<D3D9DriverList>();

		return mDriverList;
	}

	D3DPRIMITIVETYPE D3D9RenderSystem::getD3D9PrimitiveType() const
	{
		switch(mCurrentDrawOperation)
		{
		case DOT_POINT_LIST:
			return D3DPT_POINTLIST;
		case DOT_LINE_LIST:
			return D3DPT_LINELIST;
		case DOT_LINE_STRIP:
			return D3DPT_LINESTRIP;
		case DOT_TRIANGLE_LIST:
			return D3DPT_TRIANGLELIST;
		case DOT_TRIANGLE_STRIP:
			return D3DPT_TRIANGLESTRIP;
		case DOT_TRIANGLE_FAN:
			return D3DPT_TRIANGLEFAN;
		}

		return D3DPT_TRIANGLELIST;
	}

	bool D3D9RenderSystem::_checkMultiSampleQuality(D3DMULTISAMPLE_TYPE type, DWORD *outQuality, D3DFORMAT format, 
		UINT adapterNum, D3DDEVTYPE deviceType, BOOL fullScreen)
	{
		HRESULT hr;
		hr = mpD3D->CheckDeviceMultiSampleType( 
			adapterNum, 
			deviceType, 
			format, 
			fullScreen, 
			type, 
			outQuality);

		if (SUCCEEDED(hr))
			return true;
		else
			return false;
	}

	RenderSystemCapabilities* D3D9RenderSystem::updateRenderSystemCapabilities(D3D9RenderWindow* renderWindow)
	{			
		RenderSystemCapabilities* rsc = mCurrentCapabilities;
		if (rsc == NULL)
			rsc = cm_new<RenderSystemCapabilities>();

		rsc->setDriverVersion(mDriverVersion);
		rsc->setDeviceName(mActiveD3DDriver->getDriverDescription());
		rsc->setRenderSystemName(getName());

		// Init caps to maximum.		
		rsc->setCapability(RSC_ANISOTROPY);
		rsc->setCapability(RSC_AUTOMIPMAP);
		rsc->setCapability(RSC_CUBEMAPPING);			
		rsc->setCapability(RSC_TWO_SIDED_STENCIL);		
		rsc->setCapability(RSC_STENCIL_WRAP);
		rsc->setCapability(RSC_HWOCCLUSION);		
		rsc->setCapability(RSC_USER_CLIP_PLANES);			
		rsc->setCapability(RSC_VERTEX_FORMAT_UBYTE4);			
		rsc->setCapability(RSC_TEXTURE_3D);			
		rsc->setCapability(RSC_NON_POWER_OF_2_TEXTURES);
		rsc->setNumMultiRenderTargets(CM_MAX_MULTIPLE_RENDER_TARGETS);
		rsc->setCapability(RSC_MRT_DIFFERENT_BIT_DEPTHS);		
		rsc->setCapability(RSC_POINT_SPRITES);			
		rsc->setCapability(RSC_POINT_EXTENDED_PARAMETERS);								
		rsc->setMaxPointSize(10.0);
		rsc->setCapability(RSC_MIPMAP_LOD_BIAS);				
		rsc->setCapability(RSC_PERSTAGECONSTANT);
		rsc->setStencilBufferBitDepth(8);
		rsc->setCapability(RSC_ADVANCED_BLEND_OPERATIONS);

		for (UINT32 i=0; i < mDeviceManager->getDeviceCount(); ++i)
		{
			D3D9Device* device			 = mDeviceManager->getDevice(i);
			IDirect3DDevice9* d3d9Device = device->getD3D9Device();

			IDirect3DSurface9* pSurf;


			// Check for hardware stencil support
			d3d9Device->GetDepthStencilSurface(&pSurf);

			if (pSurf != NULL)
			{
				D3DSURFACE_DESC surfDesc;

				pSurf->GetDesc(&surfDesc);
				pSurf->Release();
			}																	

			// Check for hardware occlusion support
			HRESULT hr = d3d9Device->CreateQuery(D3DQUERYTYPE_OCCLUSION,  NULL);

			if (FAILED(hr))
				rsc->unsetCapability(RSC_HWOCCLUSION);
		}

		// Update RS caps using the minimum value found in adapter list.
		for (unsigned int i=0; i < mDriverList->count(); ++i)
		{
			D3D9Driver* pCurDriver       = mDriverList->item(i);			
			const D3DCAPS9& rkCurCaps    = pCurDriver->getD3D9DeviceCaps();

			rsc->setNumTextureUnits(GPT_FRAGMENT_PROGRAM, 16); // We don't support anything lower than SM3, and 16 is the sampler count determined by the specification

			rsc->setMaxBoundVertexBuffers(static_cast<UINT32>(rkCurCaps.MaxStreams));

			// Check for Anisotropy.
			if (rkCurCaps.MaxAnisotropy <= 1)
				rsc->unsetCapability(RSC_ANISOTROPY);

			// Check automatic mipmap generation.
			if ((rkCurCaps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) == 0)
				rsc->unsetCapability(RSC_AUTOMIPMAP);

			// Two-sided stencil
			if ((rkCurCaps.StencilCaps & D3DSTENCILCAPS_TWOSIDED) == 0)
				rsc->unsetCapability(RSC_TWO_SIDED_STENCIL);

			// stencil wrap
			if ((rkCurCaps.StencilCaps & D3DSTENCILCAPS_INCR) == 0 ||
				(rkCurCaps.StencilCaps & D3DSTENCILCAPS_DECR) == 0)
				rsc->unsetCapability(RSC_STENCIL_WRAP);

			// User clip planes
			if (rkCurCaps.MaxUserClipPlanes == 0)			
				rsc->unsetCapability(RSC_USER_CLIP_PLANES);			

			// UBYTE4 type?
			if ((rkCurCaps.DeclTypes & D3DDTCAPS_UBYTE4) == 0)			
				rsc->unsetCapability(RSC_VERTEX_FORMAT_UBYTE4);	

			// Check cube map support.
			if ((rkCurCaps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) == 0)
				rsc->unsetCapability(RSC_CUBEMAPPING);

			// 3D textures?
			if ((rkCurCaps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) == 0)			
				rsc->unsetCapability(RSC_TEXTURE_3D);			

			if (rkCurCaps.TextureCaps & D3DPTEXTURECAPS_POW2)
			{
				// Conditional support for non POW2
				if (!(rkCurCaps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL))							
					rsc->unsetCapability(RSC_NON_POWER_OF_2_TEXTURES);				
			}	

			// Number of render targets
			if (rkCurCaps.NumSimultaneousRTs < rsc->getNumMultiRenderTargets())
			{
				rsc->setNumMultiRenderTargets(std::min((UINT16)rkCurCaps.NumSimultaneousRTs, (UINT16)CM_MAX_MULTIPLE_RENDER_TARGETS));
			}	

			if((rkCurCaps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) == 0)
			{
				rsc->unsetCapability(RSC_MRT_DIFFERENT_BIT_DEPTHS);
			}

			// Point sprites 
			if (rkCurCaps.MaxPointSize <= 1.0f)
			{
				rsc->unsetCapability(RSC_POINT_SPRITES);
				// sprites and extended parameters go together in D3D
				rsc->unsetCapability(RSC_POINT_EXTENDED_PARAMETERS);				
			}

			// Take the minimum point size.
			if (rkCurCaps.MaxPointSize < rsc->getMaxPointSize())
				rsc->setMaxPointSize(rkCurCaps.MaxPointSize);	

			// Mipmap LOD biasing?
			if ((rkCurCaps.RasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS) == 0)			
				rsc->unsetCapability(RSC_MIPMAP_LOD_BIAS);			


			// Do we support per-stage src_manual constants?
			// HACK - ATI drivers seem to be buggy and don't support per-stage constants properly?
			// TODO: move this to RSC
			if((rkCurCaps.PrimitiveMiscCaps & D3DPMISCCAPS_PERSTAGECONSTANT) == 0)
				rsc->unsetCapability(RSC_PERSTAGECONSTANT);

			// Advanced blend operations? min max subtract rev 
			if((rkCurCaps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP) == 0)
				rsc->unsetCapability(RSC_ADVANCED_BLEND_OPERATIONS);
		}				

		// We always support compression, D3DX will decompress if device does not support
		rsc->setCapability(RSC_TEXTURE_COMPRESSION);
		rsc->setCapability(RSC_TEXTURE_COMPRESSION_DXT);

		convertVertexShaderCaps(rsc);
		convertPixelShaderCaps(rsc);

		// Adapter details
		const D3DADAPTER_IDENTIFIER9& adapterID = mActiveD3DDriver->getAdapterIdentifier();

		// determine vendor
		// Full list of vendors here: http://www.pcidatabase.com/vendors.php?sort=id
		switch(adapterID.VendorId)
		{
		case 0x10DE:
			rsc->setVendor(GPU_NVIDIA);
			break;
		case 0x1002:
			rsc->setVendor(GPU_AMD);
			break;
		case 0x163C:
		case 0x8086:
			rsc->setVendor(GPU_INTEL);
			break;
		default:
			rsc->setVendor(GPU_UNKNOWN);
			break;
		};

		rsc->setCapability(RSC_INFINITE_FAR_PLANE);

		// We always support rendertextures bigger than the frame buffer
		rsc->setCapability(RSC_HWRENDER_TO_TEXTURE);

		// Determine if any floating point texture format is supported
		D3DFORMAT floatFormats[6] = {D3DFMT_R16F, D3DFMT_G16R16F, 
			D3DFMT_A16B16G16R16F, D3DFMT_R32F, D3DFMT_G32R32F, 
			D3DFMT_A32B32G32R32F};
		IDirect3DSurface9* bbSurf;
		renderWindow->getCustomAttribute("DDBACKBUFFER", &bbSurf);
		D3DSURFACE_DESC bbSurfDesc;
		bbSurf->GetDesc(&bbSurfDesc);

		for (int i = 0; i < 6; ++i)
		{
			if (SUCCEEDED(mpD3D->CheckDeviceFormat(mActiveD3DDriver->getAdapterNumber(), 
				D3DDEVTYPE_HAL, bbSurfDesc.Format, 
				0, D3DRTYPE_TEXTURE, floatFormats[i])))
			{
				rsc->setCapability(RSC_TEXTURE_FLOAT);
				break;
			}

		}

		// Vertex textures
		if (rsc->isShaderProfileSupported("vs_3_0"))
		{
			rsc->setCapability(RSC_VERTEX_TEXTURE_FETCH);
			rsc->setNumTextureUnits(GPT_VERTEX_PROGRAM, 4);
			rsc->setNumCombinedTextureUnits(rsc->getNumTextureUnits(GPT_FRAGMENT_PROGRAM) +
				rsc->getNumTextureUnits(GPT_VERTEX_PROGRAM));
		}		
		else
		{
			rsc->setNumCombinedTextureUnits(rsc->getNumTextureUnits(GPT_FRAGMENT_PROGRAM));
		}

		// Check alpha to coverage support
		// this varies per vendor! But at least SM3 is required
		if (rsc->isShaderProfileSupported("ps_3_0"))
		{
			// NVIDIA needs a separate check
			if (rsc->getVendor() == GPU_NVIDIA)
			{
				if (mpD3D->CheckDeviceFormat(
					D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,D3DRTYPE_SURFACE, 
					(D3DFORMAT)MAKEFOURCC('A', 'T', 'O', 'C')) == S_OK)
				{
					rsc->setCapability(RSC_ALPHA_TO_COVERAGE);
				}

			}
			else if (rsc->getVendor() == GPU_AMD)
			{
				// There is no check on ATI, we have to assume SM3 == support
				rsc->setCapability(RSC_ALPHA_TO_COVERAGE);
			}

			// no other cards have Dx9 hacks for alpha to coverage, as far as I know
		}


		if (mCurrentCapabilities == NULL)
		{		
			mCurrentCapabilities = rsc;
			mCurrentCapabilities->addShaderProfile("hlsl");
			mCurrentCapabilities->addShaderProfile("cg");

			initialiseFromRenderSystemCapabilities(mCurrentCapabilities);
		}

		return rsc;
	}

	void D3D9RenderSystem::convertVertexShaderCaps(RenderSystemCapabilities* rsc) const
	{
		UINT16 major = 0xFF;
		UINT16 minor = 0xFF;
		D3DCAPS9 minVSCaps;

		// Find the device with the lowest vertex shader caps.
		for (unsigned int i=0; i < mDriverList->count(); ++i)
		{
			D3D9Driver* pCurDriver      = mDriverList->item(i);			
			const D3DCAPS9& rkCurCaps   = pCurDriver->getD3D9DeviceCaps();
			UINT16 currMajor			= static_cast<UINT16>((rkCurCaps.VertexShaderVersion & 0x0000FF00) >> 8);
			UINT16 currMinor			= static_cast<UINT16>(rkCurCaps.VertexShaderVersion & 0x000000FF);

			if (currMajor < major)	
			{
				major = currMajor;
				minor = currMinor;
				minVSCaps = rkCurCaps;
			}
			else if (currMajor == major && currMinor < minor)
			{
				minor = currMinor;
				minVSCaps = rkCurCaps;
			}			
		}

		// In case we didn't found any vertex shader support
		// try the IDirect3DDevice9 caps instead of the IDirect3D9
		// software vertex processing is reported there
		if (major == 0 && minor == 0)
		{
			IDirect3DDevice9* lpD3DDevice9 = getActiveD3D9Device();
			D3DCAPS9 d3dDeviceCaps9;
			lpD3DDevice9->GetDeviceCaps(&d3dDeviceCaps9);
			major = static_cast<UINT16>((d3dDeviceCaps9.VertexShaderVersion & 0x0000FF00) >> 8);
			minor = static_cast<UINT16>(d3dDeviceCaps9.VertexShaderVersion & 0x000000FF);
		}

		bool vs2x = false;
		bool vs2a = false;

		// Special case detection for vs_2_x/a support
		if (major >= 2)
		{
			if ((minVSCaps.VS20Caps.Caps & D3DVS20CAPS_PREDICATION) &&
				(minVSCaps.VS20Caps.DynamicFlowControlDepth > 0) &&
				(minVSCaps.VS20Caps.NumTemps >= 12))
			{
				vs2x = true;
			}

			if ((minVSCaps.VS20Caps.Caps & D3DVS20CAPS_PREDICATION) &&
				(minVSCaps.VS20Caps.DynamicFlowControlDepth > 0) &&
				(minVSCaps.VS20Caps.NumTemps >= 13))
			{
				vs2a = true;
			}
		}

		// Populate max param count
		switch (major)
		{
		case 1:
			// No boolean params allowed
			rsc->setVertexProgramConstantBoolCount(0);
			// No integer params allowed
			rsc->setVertexProgramConstantIntCount(0);
			// float params, always 4D
			rsc->setVertexProgramConstantFloatCount(static_cast<UINT16>(minVSCaps.MaxVertexShaderConst));

			break;
		case 2:
			// 16 boolean params allowed
			rsc->setVertexProgramConstantBoolCount(16);
			// 16 integer params allowed, 4D
			rsc->setVertexProgramConstantIntCount(16);
			// float params, always 4D
			rsc->setVertexProgramConstantFloatCount(static_cast<UINT16>(minVSCaps.MaxVertexShaderConst));
			break;
		case 3:
			// 16 boolean params allowed
			rsc->setVertexProgramConstantBoolCount(16);
			// 16 integer params allowed, 4D
			rsc->setVertexProgramConstantIntCount(16);
			// float params, always 4D
			rsc->setVertexProgramConstantFloatCount(static_cast<UINT16>(minVSCaps.MaxVertexShaderConst));
			break;
		}

		// populate syntax codes in program manager (no breaks in this one so it falls through)
		switch(major)
		{
		case 3:
			rsc->addShaderProfile("vs_3_0");
			rsc->addGpuProgramProfile(GPP_VS_3_0, "vs_3_0");
		case 2:
			if (vs2x)
			{
				rsc->addShaderProfile("vs_2_x");
				rsc->addGpuProgramProfile(GPP_VS_2_x, "vs_2_x");
			}
			if (vs2a)
			{
				rsc->addShaderProfile("vs_2_a");
				rsc->addGpuProgramProfile(GPP_VS_2_a, "vs_2_a");
			}

			rsc->addShaderProfile("vs_2_0");
			rsc->addGpuProgramProfile(GPP_VS_2_0, "vs_2_0");
		case 1:
			rsc->addShaderProfile("vs_1_1");
			rsc->addGpuProgramProfile(GPP_VS_1_1, "vs_1_1");
		}
	}

	void D3D9RenderSystem::convertPixelShaderCaps(RenderSystemCapabilities* rsc) const
	{
		UINT16 major = 0xFF;
		UINT16 minor = 0xFF;
		D3DCAPS9 minPSCaps;

		// Find the device with the lowest pixel shader caps.
		for (unsigned int i=0; i < mDriverList->count(); ++i)
		{
			D3D9Driver* pCurDriver      = mDriverList->item(i);			
			const D3DCAPS9& currCaps    = pCurDriver->getD3D9DeviceCaps();
			UINT16 currMajor			= static_cast<UINT16>((currCaps.PixelShaderVersion & 0x0000FF00) >> 8);
			UINT16 currMinor			= static_cast<UINT16>(currCaps.PixelShaderVersion & 0x000000FF);

			if (currMajor < major)	
			{
				major = currMajor;
				minor = currMinor;
				minPSCaps = currCaps;
			}
			else if (currMajor == major && currMinor < minor)
			{
				minor = currMinor;
				minPSCaps = currCaps;
			}			
		}

		bool ps2a = false;
		bool ps2b = false;
		bool ps2x = false;

		// Special case detection for ps_2_x/a/b support
		if (major >= 2)
		{
			if ((minPSCaps.PS20Caps.Caps & D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT) &&
				(minPSCaps.PS20Caps.NumTemps >= 32))
			{
				ps2b = true;
			}

			if ((minPSCaps.PS20Caps.Caps & D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT) &&
				(minPSCaps.PS20Caps.Caps & D3DPS20CAPS_NODEPENDENTREADLIMIT) &&
				(minPSCaps.PS20Caps.Caps & D3DPS20CAPS_ARBITRARYSWIZZLE) &&
				(minPSCaps.PS20Caps.Caps & D3DPS20CAPS_GRADIENTINSTRUCTIONS) &&
				(minPSCaps.PS20Caps.Caps & D3DPS20CAPS_PREDICATION) &&
				(minPSCaps.PS20Caps.NumTemps >= 22))
			{
				ps2a = true;
			}

			// Does this enough?
			if (ps2a || ps2b)
			{
				ps2x = true;
			}
		}

		switch (major)
		{
		case 1:
			// no boolean params allowed
			rsc->setFragmentProgramConstantBoolCount(0);
			// no integer params allowed
			rsc->setFragmentProgramConstantIntCount(0);
			// float params, always 4D
			// NB in ps_1_x these are actually stored as fixed point values,
			// but they are entered as floats
			rsc->setFragmentProgramConstantFloatCount(8);
			break;
		case 2:
			// 16 boolean params allowed
			rsc->setFragmentProgramConstantBoolCount(16);
			// 16 integer params allowed, 4D
			rsc->setFragmentProgramConstantIntCount(16);
			// float params, always 4D
			rsc->setFragmentProgramConstantFloatCount(32);
			break;
		case 3:
			// 16 boolean params allowed
			rsc->setFragmentProgramConstantBoolCount(16);
			// 16 integer params allowed, 4D
			rsc->setFragmentProgramConstantIntCount(16);
			// float params, always 4D
			rsc->setFragmentProgramConstantFloatCount(224);
			break;
		}

		// populate syntax codes in program manager (no breaks in this one so it falls through)
		switch(major)
		{
		case 3:
			if (minor > 0)
			{
				rsc->addShaderProfile("ps_3_x");
				rsc->addGpuProgramProfile(GPP_PS_3_x, "ps_3_x");
			}

			rsc->addShaderProfile("ps_3_0");
			rsc->addGpuProgramProfile(GPP_PS_3_0, "ps_3_0");
		case 2:
			if (ps2x)
			{
				rsc->addShaderProfile("ps_2_x");
				rsc->addGpuProgramProfile(GPP_PS_2_x, "ps_2_x");
			}
			if (ps2a)
			{
				rsc->addShaderProfile("ps_2_a");
				rsc->addGpuProgramProfile(GPP_PS_2_a, "ps_2_a");
			}
			if (ps2b)
			{
				rsc->addShaderProfile("ps_2_b");
				rsc->addGpuProgramProfile(GPP_PS_2_b, "ps_2_b");
			}

			rsc->addShaderProfile("ps_2_0");
			rsc->addGpuProgramProfile(GPP_PS_2_0, "ps_2_0");
		case 1:
			if (major > 1 || minor >= 4)
			{
				rsc->addShaderProfile("ps_1_4");
				rsc->addGpuProgramProfile(GPP_PS_1_4, "ps_1_4");
			}
			if (major > 1 || minor >= 3)
			{
				rsc->addShaderProfile("ps_1_3");
				rsc->addGpuProgramProfile(GPP_PS_1_3, "ps_1_3");
			}
			if (major > 1 || minor >= 2)
			{
				rsc->addShaderProfile("ps_1_2");
				rsc->addGpuProgramProfile(GPP_PS_1_2, "ps_1_2");
			}

			rsc->addShaderProfile("ps_1_1");
			rsc->addGpuProgramProfile(GPP_PS_1_1, "ps_1_1");
		}
	}

	void D3D9RenderSystem::initialiseFromRenderSystemCapabilities(RenderSystemCapabilities* caps)
	{
		if (caps->getRenderSystemName() != getName())
		{
			CM_EXCEPT(InvalidParametersException, 
				"Trying to initialize D3D9RenderSystem from RenderSystemCapabilities that do not support Direct3D9");
		}

		if (caps->isShaderProfileSupported("hlsl"))
			GpuProgramManager::instance().addFactory(mHLSLProgramFactory);

		mNumTexStages = caps->getNumCombinedTextureUnits();
		mTexStageDesc = cm_newN<sD3DTextureStageDesc>(mNumTexStages);

		// set stages desc. to defaults
		for (UINT32 n = 0; n < mNumTexStages; n++)
		{
			mTexStageDesc[n].coordIndex = 0;
			mTexStageDesc[n].texType = D3D9Mappings::D3D_TEX_TYPE_NORMAL;
			mTexStageDesc[n].pTex = 0;
			mTexStageDesc[n].pVertexTex = 0;
		}
	}

	bool D3D9RenderSystem::checkTextureFilteringSupported(TextureType ttype, PixelFormat format, int usage)
	{
		// Gets D3D format
		D3DFORMAT d3dPF = D3D9Mappings::_getPF(format);
		if (d3dPF == D3DFMT_UNKNOWN)
			return false;

		for (UINT32 i = 0; i < mDeviceManager->getDeviceCount(); ++i)
		{
			D3D9Device* currDevice = mDeviceManager->getDevice(i);
			const D3D9RenderWindow* currDevicePrimaryWindow = currDevice->getPrimaryWindow();
			IDirect3DSurface9* pSurface = currDevicePrimaryWindow->_getRenderSurface();
			D3DSURFACE_DESC srfDesc;

			// Get surface desc
			if (FAILED(pSurface->GetDesc(&srfDesc)))
				return false;

			// Calculate usage
			DWORD d3dusage = D3DUSAGE_QUERY_FILTER;
			if (usage & TU_RENDERTARGET) 
				d3dusage |= D3DUSAGE_RENDERTARGET;
			if (usage & TU_DEPTHSTENCIL) 
				d3dusage |= D3DUSAGE_DEPTHSTENCIL;
			if (usage & TU_DYNAMIC)
				d3dusage |= D3DUSAGE_DYNAMIC;

			// Detect resource type
			D3DRESOURCETYPE rtype;
			switch(ttype)
			{
			case TEX_TYPE_1D:
			case TEX_TYPE_2D:
				rtype = D3DRTYPE_TEXTURE;
				break;
			case TEX_TYPE_3D:
				rtype = D3DRTYPE_VOLUMETEXTURE;
				break;
			case TEX_TYPE_CUBE_MAP:
				rtype = D3DRTYPE_CUBETEXTURE;
				break;
			default:
				return false;
			}

			HRESULT hr = mpD3D->CheckDeviceFormat(
				currDevice->getAdapterNumber(),
				currDevice->getDeviceType(),
				srfDesc.Format,
				d3dusage,
				rtype,
				d3dPF);

			if (FAILED(hr))
				return false;
		}

		return true;		
	}

	String D3D9RenderSystem::getErrorDescription(long errorNumber) const
	{
		const String errMsg = DXGetErrorDescription(errorNumber);
		return errMsg;
	}

	void D3D9RenderSystem::setClipPlane (UINT16 index, float A, float B, float C, float D)
	{
		float plane[4] = { A, B, C, D };
		getActiveD3D9Device()->SetClipPlane (index, plane);
	}

	void D3D9RenderSystem::enableClipPlane (UINT16 index, bool enable)
	{
		DWORD prev;
		getActiveD3D9Device()->GetRenderState(D3DRS_CLIPPLANEENABLE, &prev);
		__SetRenderState(D3DRS_CLIPPLANEENABLE, enable?
			(prev | (1 << index)) : (prev & ~(1 << index)));
	}

	void D3D9RenderSystem::notifyOnDeviceLost(D3D9Device* device)
	{	
		if (mIsFrameInProgress)
		{
			endFrame();
			mRestoreFrameOnReset = true;
		}
	}

	void D3D9RenderSystem::notifyOnDeviceReset(D3D9Device* device)
	{		
		// Reset state attributes.	
		mVertexProgramBound = false;
		mFragmentProgramBound = false;

		if (mRestoreFrameOnReset)
		{
			beginFrame();
			mRestoreFrameOnReset = false;
		}
	}

	void D3D9RenderSystem::determineMultisampleSettings(IDirect3DDevice9* d3d9Device,
		UINT32 multisampleCount, const String& multisampleHint, D3DFORMAT d3dPixelFormat, 
		bool fullScreen, D3DMULTISAMPLE_TYPE *outMultisampleType, DWORD *outMultisampleQuality) const
	{
		bool ok = false;
		bool qualityHint = multisampleHint.find("Quality") != String::npos;
		UINT32 origCount = multisampleCount;

		D3D9DriverList* driverList = getDirect3DDrivers();
		D3D9Driver* deviceDriver = mActiveD3DDriver;
		D3D9Device* device = mDeviceManager->getDeviceFromD3D9Device(d3d9Device);

		for (UINT32 i = 0; i < driverList->count(); ++i)
		{
			D3D9Driver* currDriver = driverList->item(i);

			if (currDriver->getAdapterNumber() == device->getAdapterNumber())
			{
				deviceDriver = currDriver;
				break;
			}
		}

		bool tryCSAA = false;
		// NVIDIA, prefer CSAA if available for 8+
		// it would be tempting to use getCapabilities()->getVendor() == GPU_NVIDIA but
		// if this is the first window, caps will not be initialised yet
		if (deviceDriver->getAdapterIdentifier().VendorId == 0x10DE && 
			multisampleCount >= 8)
		{
			tryCSAA	 = true;
		}

		while (!ok)
		{
			// Deal with special cases
			if (tryCSAA)
			{
				// see http://developer.nvidia.com/object/coverage-sampled-aa.html
				switch(multisampleCount)
				{
				case 8:
					if (qualityHint)
					{
						*outMultisampleType = D3DMULTISAMPLE_8_SAMPLES;
						*outMultisampleQuality = 0;
					}
					else
					{
						*outMultisampleType = D3DMULTISAMPLE_4_SAMPLES;
						*outMultisampleQuality = 2;
					}
					break;
				case 16:
					if (qualityHint)
					{
						*outMultisampleType = D3DMULTISAMPLE_8_SAMPLES;
						*outMultisampleQuality = 2;
					}
					else
					{
						*outMultisampleType = D3DMULTISAMPLE_4_SAMPLES;
						*outMultisampleQuality = 4;
					}
					break;
				}
			}
			else // !CSAA
			{
				*outMultisampleType = (D3DMULTISAMPLE_TYPE)multisampleCount;
				*outMultisampleQuality = 0;
			}


			HRESULT hr;
			DWORD outQuality;
			hr = mpD3D->CheckDeviceMultiSampleType( 
				deviceDriver->getAdapterNumber(), 
				D3DDEVTYPE_HAL, 
				d3dPixelFormat, 
				fullScreen, 
				*outMultisampleType, 
				&outQuality);

			if (SUCCEEDED(hr) && 
				(!tryCSAA || outQuality > *outMultisampleQuality))
			{
				ok = true;
			}
			else
			{
				// downgrade
				if (tryCSAA && multisampleCount == 8)
				{
					// for CSAA, we'll try downgrading with quality mode at all samples.
					// then try without quality, then drop CSAA
					if (qualityHint)
					{
						// drop quality first
						qualityHint = false;
					}
					else
					{
						// drop CSAA entirely 
						tryCSAA = false;
					}
					// return to original requested samples
					multisampleCount = origCount;
				}
				else
				{
					// drop samples
					--multisampleCount;

					if (multisampleCount == 1)
					{
						// ran out of options, no multisampling
						multisampleCount = 0;
						ok = true;
					}
				}
			}

		} // while !ok
	}

	RenderSystemCapabilities* D3D9RenderSystem::createRenderSystemCapabilities() const
	{
		return mCurrentCapabilities;
	}

	void D3D9RenderSystem::setClipPlanesImpl(const PlaneList& clipPlanes)
	{
		size_t i;
		size_t numClipPlanes;
		D3DXPLANE dx9ClipPlane;
		DWORD mask = 0;
		HRESULT hr;

		numClipPlanes = clipPlanes.size();
		for (i = 0; i < numClipPlanes; ++i)
		{
			const Plane& plane = clipPlanes[i];

			dx9ClipPlane.a = plane.normal.x;
			dx9ClipPlane.b = plane.normal.y;
			dx9ClipPlane.c = plane.normal.z;
			dx9ClipPlane.d = plane.d;

			// TODO Low priority - Transform planes to clip space?

			hr = getActiveD3D9Device()->SetClipPlane(static_cast<DWORD>(i), dx9ClipPlane);
			if (FAILED(hr))
			{
				CM_EXCEPT(RenderingAPIException, "Unable to set clip plane");
			}

			mask |= (1 << i);
		}

		hr = __SetRenderState(D3DRS_CLIPPLANEENABLE, mask);
		if (FAILED(hr))
		{
			CM_EXCEPT(RenderingAPIException, "Unable to set render state for clip planes");
		}
	}

	HRESULT D3D9RenderSystem::__SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
	{
		HRESULT hr;
		DWORD oldVal;

		if ( FAILED( hr = getActiveD3D9Device()->GetRenderState(state, &oldVal) ) )
			return hr;
		if ( oldVal == value )
			return D3D_OK;
		else
			return getActiveD3D9Device()->SetRenderState(state, value);
	}

	HRESULT D3D9RenderSystem::__SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
	{
		HRESULT hr;
		DWORD oldVal;

		if ( FAILED( hr = getActiveD3D9Device()->GetSamplerState(sampler, type, &oldVal) ) )
			return hr;
		if ( oldVal == value )
			return D3D_OK;
		else
			return getActiveD3D9Device()->SetSamplerState(sampler, type, value);
	}

	HRESULT D3D9RenderSystem::__SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
	{
		HRESULT hr;
		DWORD oldVal;

		// can only set fixed-function texture stage state
		if (stage < 8)
		{
			if ( FAILED( hr = getActiveD3D9Device()->GetTextureStageState(stage, type, &oldVal) ) )
				return hr;
			if ( oldVal == value )
				return D3D_OK;
			else
				return getActiveD3D9Device()->SetTextureStageState(stage, type, value);
		}
		else
		{
			return D3D_OK;
		}
	}

	DWORD D3D9RenderSystem::_getCurrentAnisotropy(UINT32 unit)
	{
		DWORD oldVal;
		getActiveD3D9Device()->GetSamplerState(static_cast<DWORD>(unit), D3DSAMP_MAXANISOTROPY, &oldVal);
		return oldVal;
	}
}