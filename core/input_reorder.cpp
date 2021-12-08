// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#if defined(OIDN_DEVICE_SYCL)
  #include "sycl_device.h"
#endif

#include "input_reorder.h"
#include "input_reorder_ispc.h"

namespace oidn {

  InputReorderNode::InputReorderNode(const Ref<Device>& device,
                                     const std::string& name,
                                     const std::shared_ptr<Tensor>& dst,
                                     const std::shared_ptr<TransferFunction>& transferFunc,
                                     bool hdr,
                                     bool snorm)
    : Node(device, name),
      dst(dst),
      transferFunc(transferFunc),
      hdr(hdr),
      snorm(snorm)
  {
    assert(dst->ndims() == 3);
    assert(dst->layout == TensorLayout::chw ||
           dst->layout == TensorLayout::Chw8c ||
           dst->layout == TensorLayout::Chw16c);
    assert(dst->blockSize() == device->getTensorBlockSize());

    setTile(0, 0, 0, 0, 0, 0);
  }

  void InputReorderNode::setSrc(const std::shared_ptr<Image>& color, const std::shared_ptr<Image>& albedo, const std::shared_ptr<Image>& normal)
  {
    assert(dst->dims[0] >= (color  ? color->numChannels()  : 0) +
                           (albedo ? albedo->numChannels() : 0) +
                           (normal ? normal->numChannels() : 0));

    this->color  = color;
    this->albedo = albedo;
    this->normal = normal;
  }

  void InputReorderNode::setTile(int hSrc, int wSrc, int hDst, int wDst, int H, int W)
  {
    tile.hSrcBegin = hSrc;
    tile.wSrcBegin = wSrc;
    tile.hDstBegin = hDst;
    tile.wDstBegin = wDst;
    tile.H = H;
    tile.W = W;
  }

  CPUInputReorderNode::CPUInputReorderNode(const Ref<Device>& device,
                                           const std::string& name,
                                           const std::shared_ptr<Tensor>& dst,
                                           const std::shared_ptr<TransferFunction>& transferFunc,
                                           bool hdr,
                                           bool snorm)
    : InputReorderNode(device, name, dst, transferFunc, hdr, snorm) {}

  void CPUInputReorderNode::execute()
  {
    assert(tile.H + tile.hSrcBegin <= getInput()->height);
    assert(tile.W + tile.wSrcBegin <= getInput()->width);
    assert(tile.H + tile.hDstBegin <= dst->height());
    assert(tile.W + tile.wDstBegin <= dst->width());

    ispc::InputReorder impl;

    impl.color  = color  ? *color  : Image();
    impl.albedo = albedo ? *albedo : Image();
    impl.normal = normal ? *normal : Image();
    impl.dst = *dst;
    impl.tile = tile;
    impl.transferFunc = *transferFunc;
    impl.hdr = hdr;
    impl.snorm = snorm;

    parallel_nd(impl.dst.H, [&](int hDst)
    {
      ispc::InputReorder_kernel(&impl, hDst);
    });
  }

#if defined(OIDN_DEVICE_SYCL)

  template<typename T>
  struct InputReorder
  {
    // Source
    ImageAccessor<T> color;
    ImageAccessor<T> albedo;
    ImageAccessor<T> normal;

    // Destination
    TensorAccessor<half> dst;

    // Tile
    ReorderTile tile;

    // Transfer function
    TransferFunction transferFunc;
    bool hdr;
    bool snorm; // signed normalized ([-1..1])

    __forceinline void storeZero(int c, int h, int w) const
    {
      dst.set(c, h, w, 0.f);
    }

    // Stores a color value
    __forceinline void storeColor(int c, int h, int w, vec3f value) const
    {
      // Scale
      value = value * transferFunc.getInputScale();

      // Sanitize
      value = clamp(nan_to_zero(value), snorm ? -1.f : 0.f, hdr ? std::numeric_limits<float>::max() : 1.f);

      if (snorm)
      {
        // Transform to [0..1]
        value = value * 0.5f + 0.5f;
      }

      // Apply the transfer function
      value = transferFunc.forward(value);

      // Store
      dst.set3(c, h, w, value);
    }

    // Stores an albedo value
    __forceinline void storeAlbedo(int c, int h, int w, vec3f value) const
    {
      // Scale
      if (!color.ptr)
        value = value * transferFunc.getInputScale();

      // Sanitize
      value = clamp(nan_to_zero(value), 0.f, 1.f);

      // Apply the transfer function
      if (!color.ptr)
        value = transferFunc.forward(value);

      // Store
      dst.set3(c, h, w, value);
    }

    // Stores a normal value
    __forceinline void storeNormal(int c, int h, int w, vec3f value) const
    {
      // Scale
      if (!color.ptr)
        value = value * transferFunc.getInputScale();

      // Sanitize
      value = clamp(nan_to_zero(value), -1.f, 1.f);

      // Transform to [0..1]
      value = value * 0.5f + 0.5f;

      // Store
      dst.set3(c, h, w, value);
    }

    __forceinline void operator()(int hDst, int wDst) const
    {
      const int h = hDst - tile.hDstBegin;
      const int w = wDst - tile.wDstBegin;

      if (h >= 0 && h < tile.H && w >= 0 && w < tile.W)
      {
        const int hSrc = h + tile.hSrcBegin;
        const int wSrc = w + tile.wSrcBegin;
        const int wDst = w + tile.wDstBegin;

        int c = 0;

        if (color.ptr)
        {
          storeColor(c, hDst, wDst, color.get3(hSrc, wSrc));
          c += 3;
        }

        if (albedo.ptr)
        {
          storeAlbedo(c, hDst, wDst, albedo.get3(hSrc, wSrc));
          c += 3;
        }

        if (normal.ptr)
        {
          storeNormal(c, hDst, wDst, normal.get3(hSrc, wSrc));
          c += 3;
        }

        for (; c < dst.C; ++c)
          storeZero(c, hDst, wDst);
      }
      else
      {
        // Zero pad
        for (int c = 0; c < dst.C; ++c)
          storeZero(c, hDst, wDst);
      }
    }
  };

  SYCLInputReorderNode::SYCLInputReorderNode(const Ref<SYCLDevice>& device,
                                             const std::string& name,
                                             const std::shared_ptr<Tensor>& dst,
                                             const std::shared_ptr<TransferFunction>& transferFunc,
                                             bool hdr,
                                             bool snorm)
    : InputReorderNode(device, name, dst, transferFunc, hdr, snorm) {}

  void SYCLInputReorderNode::execute()
  {
    switch (getDataType(getInput()->format))
    {
    case DataType::Float32: executeKernel<float>(); break;
    case DataType::Float16: executeKernel<half>();  break;
    default:                assert(0);
    }
  }

  template<typename T>
  void SYCLInputReorderNode::executeKernel()
  {
    assert(tile.H + tile.hSrcBegin <= getInput()->height);
    assert(tile.W + tile.wSrcBegin <= getInput()->width);
    assert(tile.H + tile.hDstBegin <= dst->height());
    assert(tile.W + tile.wDstBegin <= dst->width());
    
    InputReorder<T> kernel;
    kernel.color  = color  ? *color  : Image();
    kernel.albedo = albedo ? *albedo : Image();
    kernel.normal = normal ? *normal : Image();
    kernel.dst = *dst;
    kernel.tile = tile;
    kernel.transferFunc = *transferFunc;
    kernel.hdr = hdr;
    kernel.snorm = snorm;

    auto& queue = ((SYCLDevice*)getDevice())->getSYCLQueue();
    queue.parallel_for(sycl::range<2>(dst->height(), dst->width()), [=](sycl::id<2> idx) {
      kernel(int(idx[0]), int(idx[1]));
    });
  }

#endif

} // namespace oidn