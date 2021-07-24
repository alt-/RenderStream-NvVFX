cbuffer SceneConstantBuffer : register(b0)
{
    uint iTechnique;
};

Texture2D input;
Texture2D output;
SamplerState ss
{
    Filter = MIN_MAG_MIP_LINEAR;
    AddressU = Wrap;
    AddressV = Wrap;
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    switch (iTechnique)
    {
        case 1:
            return input.Sample(ss, uv) * output.Sample(ss, uv).a;
        default:
            return output.Sample(ss, uv);
    }
}
