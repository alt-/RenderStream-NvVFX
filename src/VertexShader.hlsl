struct Output
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Output main(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    Output output;
    output.pos = pos;
    output.uv = uv;
    return output;
}
