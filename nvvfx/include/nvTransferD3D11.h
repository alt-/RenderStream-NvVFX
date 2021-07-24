/*###############################################################################
#
# Copyright(c) 2021 NVIDIA CORPORATION.All Rights Reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.
#
###############################################################################*/

#ifndef __NVTRANSFER_D3D11_H__
#define __NVTRANSFER_D3D11_H__

#include <d3d11.h>
#include "nvCVImage.h"
#include "nvTransferD3D.h"  // for NvCVImage_ToD3DFormat() and NvCVImage_FromD3DFormat()

#ifdef __cplusplus
extern "C" {
#endif // ___cplusplus



//! Initialize an NvCVImage from a D3D11 texture.
//! The pixelFormat and component types with be transferred over, and a cudaGraphicsResource will be registered;
//! the NvCVImage destructor will unregister the resource.
//! This is designed to work with NvCVImage_TransferFromArray() (and eventually NvCVImage_Transfer());
//! however it is necessary to call NvCVImage_MapResource beforehand, and NvCVImage_UnmapResource
//! before allowing D3D to render into it.
//! \param[in,out]  im  the image to be initialized.
//! \param[in]      tx  the texture to be used for initialization.
//! \return         NVCV_SUCCESS if successful.
NvCV_Status NvCV_API NvCVImage_InitFromD3D11Texture(NvCVImage *im, struct ID3D11Texture2D *tx);



#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // __NVTRANSFER_D3D11_H__

