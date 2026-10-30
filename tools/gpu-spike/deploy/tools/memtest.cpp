// memtest.exe <MB per buffer> <count>: D3D11 buffers with initial data, each copied once (the GPU uses them all),
// then the device removed reason
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
int main(int argc, char** argv) {
    UINT mb = argc > 1 ? atoi(argv[1]) : 64, count = argc > 2 ? atoi(argv[2]) : 24;
    ID3D11Device* dev; ID3D11DeviceContext* ctx; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) return 2;
    std::vector<char> data((size_t)mb << 20, 1);
    D3D11_BUFFER_DESC bd = {(UINT)data.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {data.data(), 0, 0};
    ID3D11Buffer* sink; dev->CreateBuffer(&bd, nullptr, &sink);
    std::vector<ID3D11Buffer*> bufs;
    for (UINT i = 0; i < count; i++) {
        ID3D11Buffer* b;
        HRESULT hr = dev->CreateBuffer(&bd, &init, &b);
        if (FAILED(hr)) { printf("buffer %u: 0x%08lx\n", i, hr); break; }
        bufs.push_back(b);
        ctx->CopyResource(sink, b);
        ctx->Flush();
        printf("%u MB: removed reason 0x%08lx\n", (i + 2) * mb, dev->GetDeviceRemovedReason());
        fflush(stdout);
        if (FAILED(dev->GetDeviceRemovedReason())) return 1;
    }
    // all of them in one command buffer
    for (auto* b : bufs) ctx->CopyResource(sink, b);
    ctx->Flush();
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}; ID3D11Query* q; dev->CreateQuery(&qd, &q); ctx->End(q);
    while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) Sleep(10);
    printf("all at once: removed reason 0x%08lx\n", dev->GetDeviceRemovedReason());
    return 0;
}
