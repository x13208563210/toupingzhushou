//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#include "pch.h"

namespace winrt::WindowsSample::implementation
{
    namespace
    {
        uint64_t g_request_sample_count = 0;
        uint64_t g_sample_success_count = 0;

        void TraceEvery(const char* prefix, uint64_t value)
        {
            if (value <= 5 || (value % 900) == 0)
            {
                char message[256];
                sprintf_s(message, "%s %llu", prefix, static_cast<unsigned long long>(value));
                AppendVirtualCameraTrace(message);
            }
        }
    }

    HRESULT SimpleMediaStream::Initialize(
            _In_ SimpleMediaSource* pSource,
            _In_ DWORD dwStreamId,
            _In_ MFSampleAllocatorUsage allocatorUsage
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        wil::com_ptr_nothrow<IMFMediaTypeHandler> spTypeHandler;
        wil::com_ptr_nothrow<IMFAttributes> attrs;

        RETURN_HR_IF_NULL(E_INVALIDARG, pSource);
        m_parent = pSource;

        m_dwStreamId = dwStreamId;
        m_allocatorUsage = allocatorUsage;

        const uint32_t NUM_MEDIATYPES = 1;
        wil::unique_cotaskmem_array_ptr<wil::com_ptr_nothrow<IMFMediaType>> mediaTypeList = wilEx::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(NUM_MEDIATYPES);

        // Keep the virtual camera on a single RGB32 output path so capture
        // clients do not negotiate into a less stable conversion format.
        wil::com_ptr_nothrow<IMFMediaType> spMediaType;
        RETURN_IF_FAILED(MFCreateMediaType(&spMediaType));
        RETURN_IF_FAILED(spMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED(spMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
        RETURN_IF_FAILED(spMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        RETURN_IF_FAILED(spMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        RETURN_IF_FAILED(spMediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
        RETURN_IF_FAILED(MFSetAttributeSize(
            spMediaType.get(),
            MF_MT_FRAME_SIZE,
            virtual_camera::kOutputWidth,
            virtual_camera::kOutputHeight));
        RETURN_IF_FAILED(MFSetAttributeRatio(
            spMediaType.get(),
            MF_MT_FRAME_RATE,
            virtual_camera::kOutputFps,
            1));
        RETURN_IF_FAILED(MFSetAttributeRatio(spMediaType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        RETURN_IF_FAILED(spMediaType->SetUINT32(MF_MT_DEFAULT_STRIDE, virtual_camera::kOutputStride));
        RETURN_IF_FAILED(spMediaType->SetUINT32(
            MF_MT_SAMPLE_SIZE,
            virtual_camera::kOutputWidth * virtual_camera::kOutputHeight * 4u));
        RETURN_IF_FAILED(spMediaType->SetUINT32(
            MF_MT_AVG_BITRATE,
            virtual_camera::kOutputWidth * virtual_camera::kOutputHeight * 4u * 8u * virtual_camera::kOutputFps));
        mediaTypeList[0] = spMediaType.detach();

        RETURN_IF_FAILED(MFCreateAttributes(&m_spAttributes, 10));
        RETURN_IF_FAILED(_SetStreamAttributes(m_spAttributes.get()));

        RETURN_IF_FAILED(MFCreateEventQueue(&m_spEventQueue));

        // Initialize stream descriptors
        RETURN_IF_FAILED(MFCreateStreamDescriptor(m_dwStreamId /*StreamId*/, NUM_MEDIATYPES /*MT count*/, mediaTypeList.get(), &m_spStreamDesc));

        RETURN_IF_FAILED(m_spStreamDesc->GetMediaTypeHandler(&spTypeHandler));
        RETURN_IF_FAILED(spTypeHandler->SetCurrentMediaType(mediaTypeList[0]));
        RETURN_IF_FAILED(_SetStreamDescriptorAttributes(m_spStreamDesc.get()));

        return S_OK;
    }

    // IMFMediaEventGenerator
    IFACEMETHODIMP SimpleMediaStream::BeginGetEvent(
            _In_ IMFAsyncCallback* pCallback,
            _In_ IUnknown* punkState
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->BeginGetEvent(pCallback, punkState));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::EndGetEvent(
            _In_ IMFAsyncResult* pResult,
            _COM_Outptr_ IMFMediaEvent** ppEvent
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->EndGetEvent(pResult, ppEvent));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::GetEvent(
            _In_ DWORD dwFlags,
            _COM_Outptr_ IMFMediaEvent** ppEvent
        )
    {
        // NOTE:
        // GetEvent can block indefinitely, so we don't hold the lock.
        // This requires some juggling with the event queue pointer.

        wil::com_ptr_nothrow<IMFMediaEventQueue> spQueue;

        {
            winrt::slim_lock_guard lock(m_Lock);

            RETURN_IF_FAILED(_CheckShutdownRequiresLock());
            spQueue = m_spEventQueue;
        }

        // Now get the event.
        RETURN_IF_FAILED(spQueue->GetEvent(dwFlags, ppEvent));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::QueueEvent(
            _In_ MediaEventType eventType,
            _In_ REFGUID guidExtendedType,
            _In_ HRESULT hrStatus,
            _In_opt_ PROPVARIANT const* pvValue
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(eventType, guidExtendedType, hrStatus, pvValue));

        return S_OK;
    }

    // IMFMediaStream
    IFACEMETHODIMP SimpleMediaStream::GetMediaSource(
            _COM_Outptr_ IMFMediaSource** ppMediaSource
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
        *ppMediaSource = nullptr;

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_parent.copy_to(ppMediaSource));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::GetStreamDescriptor(
            _COM_Outptr_ IMFStreamDescriptor** ppStreamDescriptor
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
        *ppStreamDescriptor = nullptr;

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        if (m_spStreamDesc != nullptr)
        {
            RETURN_IF_FAILED(m_spStreamDesc.copy_to(ppStreamDescriptor));
        }
        else
        {
            return E_UNEXPECTED;
        }

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::RequestSample(
            _In_ IUnknown* pToken
        )
    {
        winrt::slim_lock_guard lock(m_Lock);
        wil::com_ptr_nothrow<IMFSample> sample;
        wil::com_ptr_nothrow<IMFMediaBuffer> outputBuffer;
        BYTE* pbuf = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        if (m_streamState != MF_STREAM_STATE_RUNNING)
        {
            RETURN_HR_MSG(MF_E_INVALIDREQUEST, "Stream is not in running state, state:%d, selected: %d", m_streamState, m_bSelected);
        }

        TraceEvery("RequestSample", ++g_request_sample_count);
        HRESULT hr = MFCreateSample(&sample);
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "MFCreateSample failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }
        hr = MFCreateMemoryBuffer(
            virtual_camera::kOutputStride * virtual_camera::kOutputHeight,
            &outputBuffer);
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "MFCreateMemoryBuffer failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }
        hr = outputBuffer->Lock(&pbuf, &maxLength, &currentLength);
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "Buffer Lock failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }
        const HRESULT create_hr = m_spFrameGenerator->CreateFrame(
            pbuf,
            maxLength,
            static_cast<LONG>(virtual_camera::kOutputStride),
            m_rgbMask);
        const HRESULT unlock_hr = outputBuffer->Unlock();
        if (FAILED(create_hr))
        {
            char message[128];
            sprintf_s(message, "CreateFrame failed hr=0x%08lX", static_cast<unsigned long>(create_hr));
            AppendVirtualCameraTrace(message);
            return create_hr;
        }
        if (FAILED(unlock_hr))
        {
            char message[128];
            sprintf_s(message, "Buffer Unlock failed hr=0x%08lX", static_cast<unsigned long>(unlock_hr));
            AppendVirtualCameraTrace(message);
            return unlock_hr;
        }
        hr = outputBuffer->SetCurrentLength(virtual_camera::kOutputStride * virtual_camera::kOutputHeight);
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "SetCurrentLength failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }
        hr = sample->AddBuffer(outputBuffer.get());
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "AddBuffer failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }

        hr = sample->SetSampleTime(MFGetSystemTime());
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "SetSampleTime failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }
        hr = sample->SetSampleDuration(10'000'000 / virtual_camera::kOutputFps);
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "SetSampleDuration failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }
        if (pToken != nullptr)
        {
            hr = sample->SetUnknown(MFSampleExtension_Token, pToken);
            if (FAILED(hr))
            {
                char message[128];
                sprintf_s(message, "SetUnknown token failed hr=0x%08lX", static_cast<unsigned long>(hr));
                AppendVirtualCameraTrace(message);
                return hr;
            }
        }
        TraceEvery("QueueSample", ++g_sample_success_count);
        hr = m_spEventQueue->QueueEventParamUnk(MEMediaSample,
            GUID_NULL,
            S_OK,
            sample.get());
        if (FAILED(hr))
        {
            char message[128];
            sprintf_s(message, "QueueEventParamUnk failed hr=0x%08lX", static_cast<unsigned long>(hr));
            AppendVirtualCameraTrace(message);
            return hr;
        }

        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // IMFMediaStream2
    IFACEMETHODIMP SimpleMediaStream::SetStreamState(MF_STREAM_STATE state)
    {
        winrt::slim_lock_guard lock(m_Lock);
        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        if (m_streamState == state)
        {
            return S_OK;
        }

        switch (state)
        {
        case MF_STREAM_STATE_PAUSED:
            if (m_streamState != MF_STREAM_STATE_RUNNING)
            {
                return MF_E_INVALID_STATE_TRANSITION;
            }
            m_streamState = MF_STREAM_STATE_PAUSED;
            break;

        case MF_STREAM_STATE_RUNNING:
            RETURN_IF_FAILED(StartInternal(false, nullptr));
            break;

        case MF_STREAM_STATE_STOPPED:
            RETURN_IF_FAILED(StopInternal(false));

            break;

        default:
            return MF_E_INVALID_STATE_TRANSITION;
            break;
        }

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::GetStreamState(
            _Out_ MF_STREAM_STATE* pState
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        
        RETURN_HR_IF_NULL(E_INVALIDARG, pState);
        *pState = m_streamState;

        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // Public methods
    HRESULT SimpleMediaStream::Start(_In_ IMFMediaType* pMediaType)
    {
        // Set stream seleted state to true, and update current mediatype.
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_INVALIDARG, pMediaType);
        if (m_spMediaType == nullptr)
        {
            m_spMediaType = pMediaType;
        }
        m_bSelected = true;

        // Change Stream state to running.
        RETURN_IF_FAILED(StartInternal(true, pMediaType));

        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT SimpleMediaStream::Stop(_In_ bool bSendEvent)
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        m_bSelected = false;

        RETURN_IF_FAILED(StopInternal(bSendEvent));
        return S_OK;
    }

    HRESULT SimpleMediaStream::Shutdown()
    {
        winrt::slim_lock_guard lock(m_Lock);

        m_bIsShutdown = true;
        m_parent.reset();

        if (m_spEventQueue != nullptr)
        {
            m_spEventQueue->Shutdown();
            m_spEventQueue.reset();
        }

        m_spAttributes.reset();
        m_spStreamDesc.reset();

        m_streamState = MF_STREAM_STATE_STOPPED;

        return S_OK;
    }

    HRESULT SimpleMediaStream::SetSampleAllocator(IMFVideoSampleAllocator* pAllocator)
    {
        winrt::slim_lock_guard lock(m_Lock);
        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        AppendVirtualCameraTrace("SetSampleAllocator called");
        m_spSampleAllocator.reset();
        m_spSampleAllocator = pAllocator;

        return S_OK;
    }

    
    //////////////////////////////////////////////////////////////////////////////////////////
    // Private methods

    HRESULT SimpleMediaStream::_CheckShutdownRequiresLock()
    {
        if (m_bIsShutdown)
        {
            return MF_E_SHUTDOWN;
        }

        if (m_spEventQueue == nullptr)
        {
            return E_UNEXPECTED;

        }
        return S_OK;
    }

    HRESULT SimpleMediaStream::_SetStreamAttributes(
            _In_ IMFAttributes* pAttributeStore
        )
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, pAttributeStore);

        RETURN_IF_FAILED(pAttributeStore->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_dwStreamId));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

        return S_OK;
    }

    HRESULT SimpleMediaStream::_SetStreamDescriptorAttributes(
            _In_ IMFAttributes* pAttributeStore
        )
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, pAttributeStore);

        RETURN_IF_FAILED(pAttributeStore->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_dwStreamId));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT SimpleMediaStream::StartInternal(bool bSendEvent, IMFMediaType* pNewMediaType)
    {
        BOOL bMatch = FALSE;
        if (m_spMediaType && pNewMediaType)
        {
            (void)m_spMediaType->Compare(pNewMediaType, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &bMatch);

            if (!bMatch)
            {
                // update media type
                m_spMediaType = pNewMediaType;
            }
        }

        if ((m_streamState != MF_STREAM_STATE_RUNNING) || !bMatch)
        {
            UINT32 width, height;
            GUID subType;
            RETURN_IF_FAILED(m_spMediaType->GetGUID(MF_MT_SUBTYPE, &subType));
            MFGetAttributeSize(m_spMediaType.get(), MF_MT_FRAME_SIZE, &width, &height);

            DEBUG_MSG(L"Initialize frame generator for mediatype: %s, %dx%d ", winrt::to_hstring(subType).data(), width, height);
            {
                char message[256];
                sprintf_s(
                    message,
                    "StartInternal subtype=%08lX width=%lu height=%lu usage=%d",
                    static_cast<unsigned long>(subType.Data1),
                    static_cast<unsigned long>(width),
                    static_cast<unsigned long>(height),
                    static_cast<int>(m_allocatorUsage));
                AppendVirtualCameraTrace(message);
            }
            if (m_spFrameGenerator == nullptr)
            {
                m_spFrameGenerator = wil::make_unique_nothrow<SimpleFrameGenerator>();
                RETURN_IF_NULL_ALLOC_MSG(m_spFrameGenerator, "Fail to create SimpleFrameGenerator");
            }
            RETURN_IF_FAILED(m_spFrameGenerator->Initialize(m_spMediaType.get()));
        }

        if (bSendEvent)
        {
            // Post MEStreamStarted event to signal stream has started 
            RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
        }

        // Set stream state
        m_streamState = MF_STREAM_STATE_RUNNING;

        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT SimpleMediaStream::StopInternal(bool bSendEvent)
    {
        // Set stream state
        m_streamState = MF_STREAM_STATE_STOPPED;

        // NOTE: if implementation has sampleRequestQueue or sampleQueue, it must flush the queue on stopped.
        if (bSendEvent)
        {
            // Post MEStreamStopped event to signal stream has stopped
            RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
        }

        return S_OK;
    }
}
