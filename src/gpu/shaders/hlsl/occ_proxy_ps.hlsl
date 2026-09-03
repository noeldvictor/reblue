// The occlusion proxy's pixel shader: the pipeline masks every colour
// channel and writes no depth; the query counts the samples that passed.
void main(in float4 iPos : SV_Position, out float4 oC0 : SV_Target0)
{
    oC0 = float4(0.0, 0.0, 0.0, 0.0);
}
