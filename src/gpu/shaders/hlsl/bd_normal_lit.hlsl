// bd_normal_ps (0xFB83DD3F5E67CEB7), the lit scene material, as a host
// shader substituted at link time (guest_shaders.cpp, bd_host_materials).
// The recompiled body (dump of 2026-09-03, hash above) with the host's
// shadow kernel: four gathers instead of thirty texture operations. The
// rest stays as recompiled on purpose: every other path sits under a
// uniform boolean and costs nothing when the material does not take it,
// and a scene outside the census does take it (the detail textures on a
// building, found when they were dropped). The verbatim copy was
// verified indistinguishable from the guest shader before this rewrite.

#include "thirdparty/XenosRecomp/XenosRecomp/shader_common.h"


#ifdef __spirv__

#define ColorTexture_Texture2DDescriptorIndex BD_SHARED_U(0)
#define ColorTexture_Texture3DDescriptorIndex BD_SHARED_U(64)
#define ColorTexture_TextureCubeDescriptorIndex BD_SHARED_U(128)
#define ColorTexture_SamplerDescriptorIndex BD_SHARED_U(192)
#define ColorTexture1_Texture2DDescriptorIndex BD_SHARED_U(4)
#define ColorTexture1_Texture3DDescriptorIndex BD_SHARED_U(68)
#define ColorTexture1_TextureCubeDescriptorIndex BD_SHARED_U(132)
#define ColorTexture1_SamplerDescriptorIndex BD_SHARED_U(196)
#define ColorTexture2_Texture2DDescriptorIndex BD_SHARED_U(8)
#define ColorTexture2_Texture3DDescriptorIndex BD_SHARED_U(72)
#define ColorTexture2_TextureCubeDescriptorIndex BD_SHARED_U(136)
#define ColorTexture2_SamplerDescriptorIndex BD_SHARED_U(200)
#define CubeTexture_Texture2DDescriptorIndex BD_SHARED_U(20)
#define CubeTexture_Texture3DDescriptorIndex BD_SHARED_U(84)
#define CubeTexture_TextureCubeDescriptorIndex BD_SHARED_U(148)
#define CubeTexture_SamplerDescriptorIndex BD_SHARED_U(212)
#define NormalTexture_Texture2DDescriptorIndex BD_SHARED_U(16)
#define NormalTexture_Texture3DDescriptorIndex BD_SHARED_U(80)
#define NormalTexture_TextureCubeDescriptorIndex BD_SHARED_U(144)
#define NormalTexture_SamplerDescriptorIndex BD_SHARED_U(208)
#define ShadowTexture_Texture2DDescriptorIndex BD_SHARED_U(24)
#define ShadowTexture_Texture3DDescriptorIndex BD_SHARED_U(88)
#define ShadowTexture_TextureCubeDescriptorIndex BD_SHARED_U(152)
#define ShadowTexture_SamplerDescriptorIndex BD_SHARED_U(216)
#define g_vCameraPos g_PSC[1]
#define g_vColorK g_PSC[2]
#define g_vFogColor1 g_PSC[34]
#define g_vFogColor2 g_PSC[37]
#define g_vFogDir1 g_PSC[32]
#define g_vFogDir2 g_PSC[35]
#define g_vFogPos1 g_PSC[33]
#define g_vFogPos2 g_PSC[36]
#define g_vLightAmbient g_PSC[0]
#define g_vLightDiffuse1 g_PSC[22]
#define g_vLightDiffuse2 g_PSC[26]
#define g_vLightDiffuse3 g_PSC[30]
#define g_vLightDir1 g_PSC[21]
#define g_vLightDir2 g_PSC[25]
#define g_vLightDir3 g_PSC[29]
#define g_vLightParam1 g_PSC[23]
#define g_vLightParam2 g_PSC[27]
#define g_vLightParam3 g_PSC[31]
#define g_vLightPos1 g_PSC[20]
#define g_vLightPos2 g_PSC[24]
#define g_vLightPos3 g_PSC[28]
#define g_vObjectDiffuse g_PSC[3]
#define g_vObjectRefFresnel g_PSC[6]
#define g_vObjectReflect g_PSC[5]
#define g_vObjectSpecular g_PSC[4]
#define g_vShadowEpsilon g_PSC[9]
#define g_vShadowSubColor g_PSC[7]
#define s3_Texture2DDescriptorIndex BD_SHARED_U(12)
#define s3_Texture3DDescriptorIndex BD_SHARED_U(76)
#define s3_TextureCubeDescriptorIndex BD_SHARED_U(140)
#define s3_SamplerDescriptorIndex BD_SHARED_U(204)
#define s7_Texture2DDescriptorIndex BD_SHARED_U(28)
#define s7_Texture3DDescriptorIndex BD_SHARED_U(92)
#define s7_TextureCubeDescriptorIndex BD_SHARED_U(156)
#define s7_SamplerDescriptorIndex BD_SHARED_U(220)
#define s8_Texture2DDescriptorIndex BD_SHARED_U(32)
#define s8_Texture3DDescriptorIndex BD_SHARED_U(96)
#define s8_TextureCubeDescriptorIndex BD_SHARED_U(160)
#define s8_SamplerDescriptorIndex BD_SHARED_U(224)
#define s9_Texture2DDescriptorIndex BD_SHARED_U(36)
#define s9_Texture3DDescriptorIndex BD_SHARED_U(100)
#define s9_TextureCubeDescriptorIndex BD_SHARED_U(164)
#define s9_SamplerDescriptorIndex BD_SHARED_U(228)
#define s10_Texture2DDescriptorIndex BD_SHARED_U(40)
#define s10_Texture3DDescriptorIndex BD_SHARED_U(104)
#define s10_TextureCubeDescriptorIndex BD_SHARED_U(168)
#define s10_SamplerDescriptorIndex BD_SHARED_U(232)
#define s11_Texture2DDescriptorIndex BD_SHARED_U(44)
#define s11_Texture3DDescriptorIndex BD_SHARED_U(108)
#define s11_TextureCubeDescriptorIndex BD_SHARED_U(172)
#define s11_SamplerDescriptorIndex BD_SHARED_U(236)
#define s12_Texture2DDescriptorIndex BD_SHARED_U(48)
#define s12_Texture3DDescriptorIndex BD_SHARED_U(112)
#define s12_TextureCubeDescriptorIndex BD_SHARED_U(176)
#define s12_SamplerDescriptorIndex BD_SHARED_U(240)
#define s13_Texture2DDescriptorIndex BD_SHARED_U(52)
#define s13_Texture3DDescriptorIndex BD_SHARED_U(116)
#define s13_TextureCubeDescriptorIndex BD_SHARED_U(180)
#define s13_SamplerDescriptorIndex BD_SHARED_U(244)
#define s14_Texture2DDescriptorIndex BD_SHARED_U(56)
#define s14_Texture3DDescriptorIndex BD_SHARED_U(120)
#define s14_TextureCubeDescriptorIndex BD_SHARED_U(184)
#define s14_SamplerDescriptorIndex BD_SHARED_U(248)
#define s15_Texture2DDescriptorIndex BD_SHARED_U(60)
#define s15_Texture3DDescriptorIndex BD_SHARED_U(124)
#define s15_TextureCubeDescriptorIndex BD_SHARED_U(188)
#define s15_SamplerDescriptorIndex BD_SHARED_U(252)

#else

cbuffer PixelShaderConstants : register(b1, space4)
{
	float4 g_vCameraPos : packoffset(c1);
	float4 g_vColorK : packoffset(c2);
	float4 g_vFogColor1 : packoffset(c34);
	float4 g_vFogColor2 : packoffset(c37);
	float4 g_vFogDir1 : packoffset(c32);
	float4 g_vFogDir2 : packoffset(c35);
	float4 g_vFogPos1 : packoffset(c33);
	float4 g_vFogPos2 : packoffset(c36);
	float4 g_vLightAmbient : packoffset(c0);
	float4 g_vLightDiffuse1 : packoffset(c22);
	float4 g_vLightDiffuse2 : packoffset(c26);
	float4 g_vLightDiffuse3 : packoffset(c30);
	float4 g_vLightDir1 : packoffset(c21);
	float4 g_vLightDir2 : packoffset(c25);
	float4 g_vLightDir3 : packoffset(c29);
	float4 g_vLightParam1 : packoffset(c23);
	float4 g_vLightParam2 : packoffset(c27);
	float4 g_vLightParam3 : packoffset(c31);
	float4 g_vLightPos1 : packoffset(c20);
	float4 g_vLightPos2 : packoffset(c24);
	float4 g_vLightPos3 : packoffset(c28);
	float4 g_vObjectDiffuse : packoffset(c3);
	float4 g_vObjectRefFresnel : packoffset(c6);
	float4 g_vObjectReflect : packoffset(c5);
	float4 g_vObjectSpecular : packoffset(c4);
	float4 g_vShadowEpsilon : packoffset(c9);
	float4 g_vShadowSubColor : packoffset(c7);
};

cbuffer SharedConstants : register(b2, space4)
{
	uint ColorTexture_Texture2DDescriptorIndex : packoffset(c0.x);
	uint ColorTexture_Texture3DDescriptorIndex : packoffset(c4.x);
	uint ColorTexture_TextureCubeDescriptorIndex : packoffset(c8.x);
	uint ColorTexture_SamplerDescriptorIndex : packoffset(c12.x);
	uint ColorTexture1_Texture2DDescriptorIndex : packoffset(c0.y);
	uint ColorTexture1_Texture3DDescriptorIndex : packoffset(c4.y);
	uint ColorTexture1_TextureCubeDescriptorIndex : packoffset(c8.y);
	uint ColorTexture1_SamplerDescriptorIndex : packoffset(c12.y);
	uint ColorTexture2_Texture2DDescriptorIndex : packoffset(c0.z);
	uint ColorTexture2_Texture3DDescriptorIndex : packoffset(c4.z);
	uint ColorTexture2_TextureCubeDescriptorIndex : packoffset(c8.z);
	uint ColorTexture2_SamplerDescriptorIndex : packoffset(c12.z);
	uint CubeTexture_Texture2DDescriptorIndex : packoffset(c1.y);
	uint CubeTexture_Texture3DDescriptorIndex : packoffset(c5.y);
	uint CubeTexture_TextureCubeDescriptorIndex : packoffset(c9.y);
	uint CubeTexture_SamplerDescriptorIndex : packoffset(c13.y);
	uint NormalTexture_Texture2DDescriptorIndex : packoffset(c1.x);
	uint NormalTexture_Texture3DDescriptorIndex : packoffset(c5.x);
	uint NormalTexture_TextureCubeDescriptorIndex : packoffset(c9.x);
	uint NormalTexture_SamplerDescriptorIndex : packoffset(c13.x);
	uint ShadowTexture_Texture2DDescriptorIndex : packoffset(c1.z);
	uint ShadowTexture_Texture3DDescriptorIndex : packoffset(c5.z);
	uint ShadowTexture_TextureCubeDescriptorIndex : packoffset(c9.z);
	uint ShadowTexture_SamplerDescriptorIndex : packoffset(c13.z);
	uint s3_Texture2DDescriptorIndex : packoffset(c0.w);
	uint s3_Texture3DDescriptorIndex : packoffset(c4.w);
	uint s3_TextureCubeDescriptorIndex : packoffset(c8.w);
	uint s3_SamplerDescriptorIndex : packoffset(c12.w);
	uint s7_Texture2DDescriptorIndex : packoffset(c1.w);
	uint s7_Texture3DDescriptorIndex : packoffset(c5.w);
	uint s7_TextureCubeDescriptorIndex : packoffset(c9.w);
	uint s7_SamplerDescriptorIndex : packoffset(c13.w);
	uint s8_Texture2DDescriptorIndex : packoffset(c2.x);
	uint s8_Texture3DDescriptorIndex : packoffset(c6.x);
	uint s8_TextureCubeDescriptorIndex : packoffset(c10.x);
	uint s8_SamplerDescriptorIndex : packoffset(c14.x);
	uint s9_Texture2DDescriptorIndex : packoffset(c2.y);
	uint s9_Texture3DDescriptorIndex : packoffset(c6.y);
	uint s9_TextureCubeDescriptorIndex : packoffset(c10.y);
	uint s9_SamplerDescriptorIndex : packoffset(c14.y);
	uint s10_Texture2DDescriptorIndex : packoffset(c2.z);
	uint s10_Texture3DDescriptorIndex : packoffset(c6.z);
	uint s10_TextureCubeDescriptorIndex : packoffset(c10.z);
	uint s10_SamplerDescriptorIndex : packoffset(c14.z);
	uint s11_Texture2DDescriptorIndex : packoffset(c2.w);
	uint s11_Texture3DDescriptorIndex : packoffset(c6.w);
	uint s11_TextureCubeDescriptorIndex : packoffset(c10.w);
	uint s11_SamplerDescriptorIndex : packoffset(c14.w);
	uint s12_Texture2DDescriptorIndex : packoffset(c3.x);
	uint s12_Texture3DDescriptorIndex : packoffset(c7.x);
	uint s12_TextureCubeDescriptorIndex : packoffset(c11.x);
	uint s12_SamplerDescriptorIndex : packoffset(c15.x);
	uint s13_Texture2DDescriptorIndex : packoffset(c3.y);
	uint s13_Texture3DDescriptorIndex : packoffset(c7.y);
	uint s13_TextureCubeDescriptorIndex : packoffset(c11.y);
	uint s13_SamplerDescriptorIndex : packoffset(c15.y);
	uint s14_Texture2DDescriptorIndex : packoffset(c3.z);
	uint s14_Texture3DDescriptorIndex : packoffset(c7.z);
	uint s14_TextureCubeDescriptorIndex : packoffset(c11.z);
	uint s14_SamplerDescriptorIndex : packoffset(c15.z);
	uint s15_Texture2DDescriptorIndex : packoffset(c3.w);
	uint s15_Texture3DDescriptorIndex : packoffset(c7.w);
	uint s15_TextureCubeDescriptorIndex : packoffset(c11.w);
	uint s15_SamplerDescriptorIndex : packoffset(c15.w);
	DEFINE_SHARED_CONSTANTS();
};

#endif
	#define g_bDebug0 BOOL_BIT(137)
	#define g_bDebug1 BOOL_BIT(138)
	#define g_bDiffuse BOOL_BIT(135)
	#define g_bEnvMap BOOL_BIT(132)
	#define g_bFog BOOL_BIT(134)
	#define g_bFogMode1 BOOL_BIT(148)
	#define g_bFogMode2 BOOL_BIT(152)
	#define g_bNMap BOOL_BIT(131)
	#define g_bShadowMap BOOL_BIT(133)
	#define g_bSpecular BOOL_BIT(136)
	#define g_bTexture0 BOOL_BIT(128)
	#define g_bTexture1 BOOL_BIT(129)
	#define g_bTexture2 BOOL_BIT(130)

#ifndef __spirv__
[shader("pixel")]
#endif
void main(
	in float4 iPos : SV_Position,
	in float4 iTexCoord0 : TEXCOORD0,
	in float4 iTexCoord1 : TEXCOORD1,
	in float4 iTexCoord2 : TEXCOORD2,
	in float4 iTexCoord3 : TEXCOORD3,
	in float4 iTexCoord4 : TEXCOORD4,
	in float4 iTexCoord5 : TEXCOORD5,
	in float4 iTexCoord6 : TEXCOORD6,
	in float4 iTexCoord7 : TEXCOORD7,
	in float4 iTexCoord8 : TEXCOORD8,
	in float4 iTexCoord9 : TEXCOORD9,
	in float4 iTexCoord10 : TEXCOORD10,
	in float4 iTexCoord11 : TEXCOORD11,
	in float4 iTexCoord12 : TEXCOORD12,
	in float4 iTexCoord13 : TEXCOORD13,
	in float4 iTexCoord14 : TEXCOORD14,
	in float4 iTexCoord15 : TEXCOORD15,
	in centroid float4 iColor0 : COLOR0,
	in centroid float4 iColor1 : COLOR1,
#ifdef __spirv__
	in bool iFace : SV_IsFrontFace
#else
	in uint iFace : SV_IsFrontFace
#endif
#ifdef __spirv__
	,in uint iViewID : SV_ViewID
#endif
,
	out float4 oC0 : SV_Target0)
{
#ifdef __spirv__
	g_ViewIndex = iViewID;
#endif
	float4 c248 = float4(asfloat(0x0u), asfloat(0x0u), asfloat(0x0u), asfloat(0x0u));
	float4 c249 = float4(asfloat(0x0u), asfloat(0x0u), asfloat(0x0u), asfloat(0x0u));
	float4 c250 = float4(asfloat(0x40200000u), asfloat(0x3F000000u), (0.5 + (asfloat(0x3F00547Bu) - 0.5) * g_ShadowPcfScale), asfloat(0x3E2AAAABu));
	float4 c251 = float4(asfloat(0x40000000u), asfloat(0x3ECCCCCDu), asfloat(0xBF800000u), (0.5 + (asfloat(0x3F002A3Du) - 0.5) * g_ShadowPcfScale));
	float4 c252 = float4(asfloat(0x3F800000u), asfloat(0x0u), (asfloat(0x3A800000u) * g_ShadowPcfScale), (0.5 + (asfloat(0x3EFF570Au) - 0.5) * g_ShadowPcfScale));
	float4 c253 = float4(asfloat(0x3F866666u), asfloat(0x3F7851ECu), asfloat(0x3FA28F5Cu), asfloat(0x3FC00000u));
	float4 c254 = float4((1 + (asfloat(0x3F7FC000u) - 1) * g_ShadowPcfScale), (1 + (asfloat(0x3F7FD5C3u) - 1) * g_ShadowPcfScale), (1 + (asfloat(0x3F800A8Fu) - 1) * g_ShadowPcfScale), (1 + (asfloat(0x3F804000u) - 1) * g_ShadowPcfScale));
	float4 c255 = float4((0.5 + (asfloat(0x3EFFAB85u) - 0.5) * g_ShadowPcfScale), asfloat(0x0u), asfloat(0x0u), asfloat(0x0u));

	float4 r0 = iTexCoord0;
	float4 r1 = iTexCoord1;
	float4 r2 = iTexCoord2;
	float4 r3 = iTexCoord3;
	float4 r4 = iTexCoord4;
	float4 r5 = iTexCoord5;
	float4 r6 = iTexCoord6;
	float4 r7 = iColor0;
	float4 r8 = 0.0;
	float4 r9 = 0.0;
	float4 r10 = 0.0;
	float4 r11 = 0.0;
	float4 r12 = 0.0;
	float4 r13 = 0.0;
	float4 r14 = 0.0;
	float4 r15 = 0.0;
	float4 r16 = 0.0;
	float4 r17 = 0.0;
	float4 r18 = 0.0;
	float4 r19 = 0.0;
	float4 r20 = 0.0;
	float4 r21 = 0.0;
	float4 r22 = 0.0;
	float4 r23 = 0.0;
	float4 r24 = 0.0;
	float4 r25 = 0.0;
	float4 r26 = 0.0;
	float4 r27 = 0.0;
	float4 r28 = 0.0;
	float4 r29 = 0.0;
	float4 r30 = 0.0;
	float4 r31 = 0.0;
	int a0 = 0;
	int aL = 0;
	bool p0 = false;
	float ps = 0.0;
	CubeMapData cubeMapData = (CubeMapData)0;
	float2 shadowTapUV[8] = (float2[8])0;

	r4.w = dot(r4.zxy, r4.zxy);
	r9.xyz = -r2.xyz + g_vCameraPos.xyz;
	r8.x = dot(r3.zxy, r3.zxy);
	r3.w = dot(r9.zxy, r9.zxy);
	ps = clamp(rsqrt(abs(r8.x)), FLT_MIN, FLT_MAX);
	r8.x = ps;
	r8.xyz = r8.xxx * r3.zyx;
	ps = clamp(rsqrt(abs(r4.w)), FLT_MIN, FLT_MAX);
	r3.x = ps;
	r4.xyz = r3.xxx * r4.zyx;
	ps = clamp(rsqrt(abs(r3.w)), FLT_MIN, FLT_MAX);
	r3.x = ps;
	r9.xyz = r9.zyx * r3.xxx;
	if (g_bNMap)
	{
		r3.xyz = tfetch2D(NormalTexture_Texture2DDescriptorIndex, NormalTexture_SamplerDescriptorIndex, r1.zw, float2(0, 0)).xyz;
		r10.xyz = r3.yzx * c251.xxx + c251.zzz;
		r3.xy = r8.xx * r4.zy;
		r11.xyzw = r8.yyzz * r4.xzyx;
		r3.z = r11.x + -r3.y;
		ps = max(r3.x, r3.x);
		r3.x = r11.z + -r11.y;
		ps = -r11.w + ps;
		r3.y = ps;
	}
	if (g_bNMap)
	{
		r3.xyz = r10.xxx * r3.xyz;
		r3.xyz = r4.zxy * r10.zzz + r3.zxy;
		r3.yzw = r8.yzx * r10.yyy + r3.zxy;
		r3.x = dot(r3.wzy, r3.wzy);
		ps = clamp(rsqrt(abs(r3.x)), FLT_MIN, FLT_MAX);
		r3.x = ps;
		r8.xyz = r3.wyz * r3.xxx;
	}
	if (!g_bTexture0)
	{
		r10.xyzw = max(c252.xxxx, c252.xxxx);
	}
	else
	{
		ps = -abs(r7.x) > 0.0;
		r3.x = ps;
		if (g_bEnvMap)
		{
			r3.x = r9.x * r8.x;
			r3.y = -r9.x * r8.x;
			r10.x = r9.z * r8.z;
			r3.z = r9.y * r8.y;
			ps = max(-r9.z, -r9.z);
			r3.w = -r9.y * r8.y;
			ps = r8.z * ps;
			r10.y = ps;
			r3.zw = r10.xy + r3.zw;
			r10.xy = r3.zw + r3.xy;
			ps = r10.y + r10.y;
			r1.z = ps;
			r3.xyz = -r1.zzz * r8.yxz + -r9.yxz;
			r3.xyzw = cube(r3.zxyy, cubeMapData);
			r11.z = max(r3.w, r3.w);
			ps = clamp(rcp(abs(r3.z)), FLT_MIN, FLT_MAX);
			r1.z = ps;
			r11.xy = r3.yx * r1.zz + c253.ww;
			r3.xyzw = tfetchCube(CubeTexture_TextureCubeDescriptorIndex, CubeTexture_SamplerDescriptorIndex, r11.xyz, cubeMapData).xyzw;
			r4.w = -g_vObjectRefFresnel.x + c252.x;
			r1.z = max(r10.x, c252.y);
			ps = c252.x - r1.z;
			r1.z = ps;
			r10.xyzw = r3.xyzw * g_vObjectReflect.xyzw;
			ps = clamp(log2(abs(r1.z)), FLT_MIN, FLT_MAX);
			r1.z = ps;
			ps = g_vObjectRefFresnel.y * r1.z;
			r3.w = ps;
			r3.xyz = r10.xyz * g_vObjectRefFresnel.zzz;
			ps = exp2(r3.w);
			r3.w = ps;
			r3.w = r4.w * r3.w + g_vObjectRefFresnel.x;
			r3.w = max(r3.w, c252.y);
			r3.w = r10.w * r3.w;
		}
		else
		{
			r3.yzw = max(r3.xxx, r3.xxx);
		}
		r10.xyzw = tfetch2D(ColorTexture_Texture2DDescriptorIndex, ColorTexture_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyzw;
		r10.xyzw = select(-r0.xxxx > 0.0, r3.wzyx, r10.wzyx);
		if (g_bTexture1)
		{
			r11.xyzw = tfetch2D(ColorTexture1_Texture2DDescriptorIndex, ColorTexture1_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyzw;
			r0.xyzw = select(-r0.zzzz > 0.0, r3.xyzw, r11.xyzw);
			r11.xyz = -r10.ywz + r0.zxy;
			r10.yzw = r11.xzy * r0.www + r10.yzw;
			if (g_bTexture2)
			{
				r0.xyzw = tfetch2D(ColorTexture2_Texture2DDescriptorIndex, ColorTexture2_SamplerDescriptorIndex, r1.xy, float2(0, 0)).xyzw;
				r0.xyzw = select(-r1.xxxx > 0.0, r3.xyzw, r0.xyzw);
				r1.xyz = -r10.ywz + r0.zxy;
				r10.yzw = r1.xzy * r0.www + r10.yzw;
			}
		}
	}
	r0.xyzw = r10.xyzw * g_vObjectDiffuse.wzyx;
	r1.xyzw = r0.wzyx * r7.xyzw;
	ps = max(c252.x, c252.x);
	r7.y = ps;
	if (g_bShadowMap)
	{
		// The host shadow kernel (2026-09-03). The guest's was six depth fetches
		// and six four-load compares, thirty texture operations a fragment, on
		// taps spread +-1.3/1024 of the map times g_ShadowPcfScale (the host
		// holds that penumbra constant in world space, constant_buffers.h).
		// Four GatherRed calls of the D32 map at the corners of a quad half that
		// wide, each a bilinear compare, cover the same penumbra with sixteen
		// texels for four fetches. The projection (uv from the second set, v
		// flipped as D3D does), the depth-proportional and slope-scaled biases
		// and the "outside the map is lit" rule are the recompiled ones; r7.y
		// leaves this block as the lit fraction, which the diffuse block consumes.
		ps = clamp(rcp(r5.w), FLT_MIN, FLT_MAX);
		r7.x = ps;
		r3.yzw = r7.xxx * r5.zyx;
		r7.y = saturate(dot(r8.xzy, -g_vLightDir1.zxy));
		ps = clamp(rcp(r6.w), FLT_MIN, FLT_MAX);
		r7.x = ps;
		r0.zw = r6.yx * c250.yy * r7.xx;
		r3.x = c252.x - r7.y;
		r5.xyzw = r3.xyzw * g_vShadowEpsilon.xxwz;
		float shadow_ref = r7.x * r6.z - r5.y - r5.x * c251.y;
		float2 shadow_uv = float2(c250.y + r0.w, c250.y - r0.z);
		BD_TEX2D shadow_tex = g_Texture2DDescriptorHeap[ShadowTexture_Texture2DDescriptorIndex];
		float2 shadow_dim = float2(getTexture2DDimensions(shadow_tex));
		float shadow_o = 0.65 * c252.z; // c252.z is (1/1024) * g_ShadowPcfScale
		r7.y = 0.0;
		[unroll] for (int shadow_i = 0; shadow_i < 4; ++shadow_i)
		{
			float2 tap_uv = shadow_uv + float2((shadow_i & 1) ? shadow_o : -shadow_o, (shadow_i & 2) ? shadow_o : -shadow_o);
			float4 shadow_taps = shadow_tex.GatherRed(g_SamplerDescriptorHeap[ShadowTexture_SamplerDescriptorIndex], BD_UV(tap_uv));
			float4 shadow_lit = select(shadow_taps > shadow_ref.xxxx, float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0));
			float2 shadow_f = frac(tap_uv * shadow_dim - 0.5);
			r7.y += 0.25 * lerp(lerp(shadow_lit.w, shadow_lit.z, shadow_f.x), lerp(shadow_lit.x, shadow_lit.y, shadow_f.x), shadow_f.y);
		}
		if (any(shadow_uv < 0.0) || any(shadow_uv > 1.0))
			r7.y = c252.x;
	}
	r7.x = c250.y > g_vLightPos1.w;
	p0 = r7.x == 0.0;
	ps = p0 ? 0.0 : 1.0;
	if (p0)
	{
		r7.x = c253.w > g_vLightPos1.w;
		p0 = r7.x != 0.0;
		ps = p0 ? 0.0 : 1.0;
		if (p0)
		{
			r0.xyz = max(-g_vLightDir1.zyx, -g_vLightDir1.zyx);
			ps = max(c252.x, c252.x);
			r7.z = ps;
		}
		else
		{
			r7.w = c250.x > g_vLightPos1.w;
			r3.xyz = r2.xyz + -g_vLightPos1.xyz;
			r0.xyz = -r2.xyz + g_vLightPos1.xyz;
			r7.z = dot(r0.xyz, r0.xyz);
			r7.x = dot(r3.zxy, r3.zxy);
			ps = clamp(rsqrt(abs(r7.z)), FLT_MIN, FLT_MAX);
			r7.z = ps;
			r0.xyz = r0.zyx * r7.zzz;
			ps = sqrt(abs(r7.x));
			r7.x = ps;
			ps = saturate(g_vLightDiffuse1.w * r7.x);
			r7.x = ps;
			r7.z = -r7.x + c252.x;
			p0 = r7.w != 0.0;
			ps = p0 ? 0.0 : 1.0;
			if (p0)
			{
				r7.x = -g_vLightParam1.x + c252.x;
				r7.w = dot(-r0.xzy, g_vLightDir1.zxy);
				ps = clamp(rcp(r7.x), FLT_MIN, FLT_MAX);
				r7.x = ps;
				r7.w = saturate(r7.w + -g_vLightParam1.x);
				ps = g_vLightDir1.w * r7.x;
				r7.x = ps;
				r7.x = saturate(r7.x * r7.w);
				r7.z = r7.x * r7.z;
			}
		}
		r3.xyz = r9.xyz + r0.xyz;
		r7.w = dot(r3.xzy, r3.xzy);
		r7.x = dot(r8.xzy, r0.xzy);
		ps = clamp(rsqrt(abs(r7.w)), FLT_MIN, FLT_MAX);
		r7.w = ps;
		r0.xyz = r3.xyz * r7.www;
		r7.w = dot(r0.xzy, r8.xzy);
		r0.yz = max(r7.xw, c252.yy);
		ps = clamp(log2(abs(r0.z)), FLT_MIN, FLT_MAX);
		r7.x = ps;
		ps = g_vObjectSpecular.w * r7.x;
		r7.x = ps;
		ps = exp2(r7.x);
		r0.x = ps;
		r3.xy = r0.xy * r7.zz;
	}
	else
	{
		r3.xy = -abs(r7.xx) > c252.yy;
	}
	r7.x = c250.y > g_vLightPos2.w;
	p0 = r7.x != 0.0;
	ps = p0 ? 0.0 : 1.0;
	if (p0)
	{
		r7.zw = -abs(r7.xx) > c252.yy;
	}
	else
	{
		r7.x = c253.w > g_vLightPos2.w;
		p0 = r7.x != 0.0;
		ps = p0 ? 0.0 : 1.0;
		if (p0)
		{
			r0.xyz = max(-g_vLightDir2.zyx, -g_vLightDir2.zyx);
			ps = max(c252.x, c252.x);
			r7.z = ps;
		}
		else
		{
			r7.w = c250.x > g_vLightPos2.w;
			r5.xyz = r2.xyz + -g_vLightPos2.xyz;
			r0.xyz = -r2.xyz + g_vLightPos2.xyz;
			r7.z = dot(r0.xyz, r0.xyz);
			r7.x = dot(r5.zxy, r5.zxy);
			ps = clamp(rsqrt(abs(r7.z)), FLT_MIN, FLT_MAX);
			r7.z = ps;
			r0.xyz = r0.zyx * r7.zzz;
			ps = sqrt(abs(r7.x));
			r7.x = ps;
			ps = saturate(g_vLightDiffuse2.w * r7.x);
			r7.x = ps;
			r7.z = -r7.x + c252.x;
			p0 = r7.w != 0.0;
			ps = p0 ? 0.0 : 1.0;
			if (p0)
			{
				r7.x = -g_vLightParam2.x + c252.x;
				r7.w = dot(-r0.xzy, g_vLightDir2.zxy);
				ps = clamp(rcp(r7.x), FLT_MIN, FLT_MAX);
				r7.x = ps;
				r7.w = saturate(r7.w + -g_vLightParam2.x);
				ps = g_vLightDir2.w * r7.x;
				r7.x = ps;
				r7.x = saturate(r7.x * r7.w);
				r7.z = r7.x * r7.z;
			}
		}
		r5.xyz = r9.xyz + r0.xyz;
		r7.w = dot(r5.xzy, r5.xzy);
		r7.x = dot(r8.xzy, r0.xzy);
		ps = clamp(rsqrt(abs(r7.w)), FLT_MIN, FLT_MAX);
		r7.w = ps;
		r0.xyz = r5.xyz * r7.www;
		r7.w = dot(r0.xzy, r8.xzy);
		r0.yz = max(r7.xw, c252.yy);
		ps = clamp(log2(abs(r0.z)), FLT_MIN, FLT_MAX);
		r7.x = ps;
		ps = g_vObjectSpecular.w * r7.x;
		r7.x = ps;
		ps = exp2(r7.x);
		r0.x = ps;
		r7.zw = r0.xy * r7.zz;
	}
	r7.x = c250.y > g_vLightPos3.w;
	p0 = r7.x != 0.0;
	ps = p0 ? 0.0 : 1.0;
	if (p0)
	{
		r0.xy = -abs(r7.xx) > c252.yy;
	}
	else
	{
		r7.x = c253.w > g_vLightPos3.w;
		p0 = r7.x != 0.0;
		ps = p0 ? 0.0 : 1.0;
		if (p0)
		{
			r0.xyz = max(-g_vLightDir3.zyx, -g_vLightDir3.zyx);
			ps = max(c252.x, c252.x);
			r0.w = ps;
		}
		else
		{
			r3.z = c250.x > g_vLightPos3.w;
			r5.xyz = r2.xyz + -g_vLightPos3.xyz;
			r0.yzw = -r2.xyz + g_vLightPos3.xyz;
			r0.x = dot(r0.yzw, r0.yzw);
			r7.x = dot(r5.zxy, r5.zxy);
			ps = clamp(rsqrt(abs(r0.x)), FLT_MIN, FLT_MAX);
			r0.x = ps;
			r0.xyz = r0.wzy * r0.xxx;
			ps = sqrt(abs(r7.x));
			r7.x = ps;
			ps = saturate(g_vLightDiffuse3.w * r7.x);
			r7.x = ps;
			r0.w = -r7.x + c252.x;
			p0 = r3.z != 0.0;
			ps = p0 ? 0.0 : 1.0;
			if (p0)
			{
				r7.x = -g_vLightParam3.x + c252.x;
				r3.z = dot(-r0.xzy, g_vLightDir3.zxy);
				ps = clamp(rcp(r7.x), FLT_MIN, FLT_MAX);
				r7.x = ps;
				r3.z = saturate(r3.z + -g_vLightParam3.x);
				ps = g_vLightDir3.w * r7.x;
				r7.x = ps;
				r7.x = saturate(r7.x * r3.z);
				r0.w = r7.x * r0.w;
			}
		}
		r5.xyz = r9.xyz + r0.xyz;
		r7.x = dot(r5.xzy, r5.xzy);
		r0.x = dot(r8.xzy, r0.xzy);
		ps = clamp(rsqrt(abs(r7.x)), FLT_MIN, FLT_MAX);
		r7.x = ps;
		r5.xyz = r5.xyz * r7.xxx;
		r0.y = dot(r5.xzy, r8.xzy);
		r0.yz = max(r0.xy, c252.yy);
		ps = clamp(log2(abs(r0.z)), FLT_MIN, FLT_MAX);
		r7.x = ps;
		ps = g_vObjectSpecular.w * r7.x;
		r7.x = ps;
		ps = exp2(r7.x);
		r0.x = ps;
		r0.xy = r0.xy * r0.ww;
	}
	if (g_bDiffuse)
	{
		r3.yzw = r3.yyy * g_vLightDiffuse1.xyz;
		r5.xyz = r3.yzw + g_vLightAmbient.xyz;
		ps = c252.x - r7.y;
		r7.x = ps;
		r3.yzw = r3.zwy * r7.xxx + -r7.xxx;
		r3.yzw = r3.zwy * g_vShadowSubColor.www + r7.xxx;
		r5.xyz = r7.www * g_vLightDiffuse2.zxy + r5.zxy;
		r0.yzw = r0.yyy * g_vLightDiffuse3.yzx + r5.zxy;
	}
	if (g_bDiffuse)
	{
		r0.yzw = -r3.zwy * g_vShadowSubColor.xyz + r0.wyz;
		r1.xyz = r1.xyz * r0.yzw;
	}
	if (g_bSpecular)
	{
		r7.w = r3.x * r7.y;
		r7.xyz = r7.zzz * g_vLightDiffuse2.xyz;
		r7.xyz = r7.www * g_vLightDiffuse1.zxy + r7.zxy;
		r7.xyz = r0.xxx * g_vLightDiffuse3.yzx + r7.zxy;
		r7.xyz = r7.zxy * c253.xyz;
		r1.xyz = r7.xyz * g_vObjectSpecular.xyz + r1.xyz;
	}
	if (g_bFog)
	{
		if (!g_bFogMode1)
		{
			r7.x = g_vFogPos1.w + -g_vFogDir1.w;
			ps = clamp(rcp(r7.x), FLT_MIN, FLT_MAX);
			r7.x = ps;
			if (BOOL_BIT(149))
			{
				r7.yzw = r2.xyz + -g_vCameraPos.xyz;
				r7.y = dot(r7.wyz, r7.wyz);
				ps = sqrt(abs(r7.y));
				r7.y = ps;
				r7.y = r7.y + -g_vFogDir1.w;
				r7.x = saturate(r7.y * r7.x);
				r7.xyzw = r7.xxxx * g_vFogColor1.zyxw;
			}
			else
			{
				r7.yzw = r2.zyx + -g_vFogPos1.zyx;
				r7.y = dot(r7.ywz, g_vFogDir1.zxy);
				r7.y = r7.y + -g_vFogDir1.w;
				r7.x = saturate(r7.y * r7.x);
				r7.xyzw = r7.xxxx * g_vFogColor1.zyxw;
			}
		}
		else
		{
			r7.xyzw = -abs(r7.xxxx) > c252.yyyy;
		}
		if (BOOL_BIT(150))
		{
			r7.xyz = r7.xyz + -r1.zyx;
			r0.xyz = r7.xyz * r7.www + r1.zyx;
		}
		else
		{
			r7.xyz = r7.xyz * r7.www;
			if (BOOL_BIT(151))
			{
				r0.xyz = r7.xyz + r1.zyx;
			}
			else
			{
				r0.xyz = -r7.xyz + r1.zyx;
			}
		}
		if (!g_bFogMode2)
		{
			r7.x = g_vFogPos2.w + -g_vFogDir2.w;
			ps = clamp(rcp(r7.x), FLT_MIN, FLT_MAX);
			r7.x = ps;
			if (BOOL_BIT(153))
			{
				r7.yzw = r2.xyz + -g_vCameraPos.xyz;
				r7.y = dot(r7.wyz, r7.wyz);
				ps = sqrt(abs(r7.y));
				r7.y = ps;
				r7.y = r7.y + -g_vFogDir2.w;
				r7.x = saturate(r7.y * r7.x);
				r7.xyzw = r7.xxxx * g_vFogColor2.zyxw;
			}
			else
			{
				r7.yzw = r2.zyx + -g_vFogPos2.zyx;
				r7.y = dot(r7.ywz, g_vFogDir2.zxy);
				r7.y = r7.y + -g_vFogDir2.w;
				r7.x = saturate(r7.y * r7.x);
				r7.xyzw = r7.xxxx * g_vFogColor2.zyxw;
			}
		}
		else
		{
			r7.xyzw = -abs(r7.xxxx) > c252.yyyy;
		}
		if (BOOL_BIT(154))
		{
			r7.xyz = r7.xyz + -r0.xyz;
			r1.xyz = r7.zyx * r7.www + r0.zyx;
		}
		else
		{
			r7.xyz = r7.xyz * r7.www;
			if (BOOL_BIT(155))
			{
				r1.xyz = r7.zyx + r0.zyx;
			}
			else
			{
				r1.xyz = -r7.zyx + r0.zyx;
			}
		}
	}
	r7.xyz = r1.xyz + g_vColorK.xyz;
	r1.xyz = r7.zyx * g_vColorK.www;
	if (g_bDebug0)
	{
		if (g_bDebug1)
		{
			r1.z = dot(r8.xzy, g_vLightDir1.zxy);
			ps = max(-r1.z, -r1.z);
			r1.y = ps;
		}
		if (!g_bDebug1)
		{
			r1.z = dot(r4.xzy, g_vLightDir1.zxy);
			ps = max(-r1.z, -r1.z);
			r1.y = ps;
		}
		r1.xw = max(c252.yx, c252.yx);
	}
	oC0.xyzw = max(r1.zyxw, r1.zyxw);
	[branch] if (g_SpecConstants() & SPEC_CONSTANT_CEL)		oC0.xyz = BD_CelBand(oC0.xyz);
	[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)	{		clip(oC0.w - g_AlphaThreshold);
	}	return;
}