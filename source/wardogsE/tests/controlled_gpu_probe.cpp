// Appended to a generated copy of gpu.cpp for this test only.
// Read the actual D3D11 composition back buffer after Direct2D has finished.
extern "C" int controlled_gpu_pixels(unsigned char* pixels, int width, int height) {
    if (!g_ready || width != g_w || height != g_h) return 0;
    if (g_batch) {
        g_d2dc->SetTarget(NULL);
        HRESULT end = g_d2dc->EndDraw();
        g_batch = 0;
        if (FAILED(end)) return 0;
    }
    ID3D11Texture2D* back = NULL;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return 0;
    D3D11_TEXTURE2D_DESC description;
    back->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ID3D11Texture2D* staging = NULL;
    HRESULT result = g_dev->CreateTexture2D(&description, NULL, &staging);
    if (SUCCEEDED(result)) {
        g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped;
        result = g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(result)) {
            for (int y = 0; y < height; ++y)
                memcpy(pixels + (size_t)y * width * 4,
                       (const unsigned char*)mapped.pData + (size_t)y * mapped.RowPitch,
                       (size_t)width * 4);
            g_ctx->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
    return SUCCEEDED(result) && SUCCEEDED(g_dev->GetDeviceRemovedReason());
}
