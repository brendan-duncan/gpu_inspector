;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; SV_Position              0   xyzw        0      POS   float       
; COLOR                    0   xyz         1     NONE   float   xyz 
; TEXCOORD                 0   xy          2     NONE   float   xy  
;
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; SV_Target                0   xyzw        0   TARGET   float   xyzw
;
; shader debug name: 7c429051645c3f726183ec5e7e49a148.pdb
; shader hash: 7c429051645c3f726183ec5e7e49a148
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Pixel Shader
; DepthOutput=0
; SampleFrequency=0
; MinimumExpectedWaveLaneCount: 0
; MaximumExpectedWaveLaneCount: 4294967295
; UsesViewID: false
; SigInputElements: 3
; SigOutputElements: 1
; SigPatchConstOrPrimElements: 0
; SigInputVectors: 3
; SigOutputVectors[0]: 1
; SigOutputVectors[1]: 0
; SigOutputVectors[2]: 0
; SigOutputVectors[3]: 0
; EntryFunctionName: PSMain
;
;
; Input signature:
;
; Name                 Index             InterpMode DynIdx
; -------------------- ----- ---------------------- ------
; SV_Position              0          noperspective       
; COLOR                    0                 linear       
; TEXCOORD                 0                 linear       
;
; Output signature:
;
; Name                 Index             InterpMode DynIdx
; -------------------- ----- ---------------------- ------
; SV_Target                0                              
;
; Buffer Definitions:
;
; cbuffer Frame
; {
;
;   struct Frame
;   {
;
;       float time;                                   ; Offset:    0
;       uint flags;                                   ; Offset:    4
;   
;   } Frame;                                          ; Offset:    0 Size:     8
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; Frame                             cbuffer      NA          NA     CB0            cb1     1
; pointSampler                      sampler      NA          NA      S0             s0     1
; checker                           texture     f32          2d      T0             t0     1
;
;
; ViewId state:
;
; Number of inputs: 10, outputs: 4
; Outputs dependent on ViewId: {  }
; Inputs contributing to computation of Outputs:
;   output 0 depends on inputs: { 4, 8, 9 }
;   output 1 depends on inputs: { 5, 8, 9 }
;   output 2 depends on inputs: { 6, 8, 9 }
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResRet.f32 = type { float, float, float, float, i32 }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%"class.Texture2D<vector<float, 4> >" = type { <4 x float>, %"class.Texture2D<vector<float, 4> >::mips_type" }
%"class.Texture2D<vector<float, 4> >::mips_type" = type { i32 }
%Frame = type { float, i32 }
%struct.SamplerState = type { i32 }

define void @PSMain() {
.lr.ph3:
  %checker_texture_2d = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false), !dbg !97 ; line:47 col:20  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %pointSampler_sampler = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 3, i32 0, i32 0, i1 false), !dbg !97 ; line:47 col:20  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %Frame_cbuffer = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 1, i1 false), !dbg !97 ; line:47 col:20  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %0 = call float @dx.op.loadInput.f32(i32 4, i32 2, i32 0, i8 0, i32 undef), !dbg !105 ; line:53 col:23  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  %1 = call float @dx.op.loadInput.f32(i32 4, i32 2, i32 0, i8 1, i32 undef), !dbg !105 ; line:53 col:23  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  call void @llvm.dbg.value(metadata float %25, i64 0, metadata !106, metadata !107), !dbg !105 ; var:"input" !DIExpression(DW_OP_bit_piece, 128, 32) func:"PSMain"
  %2 = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 1, i32 undef), !dbg !105 ; line:53 col:23  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  call void @llvm.dbg.value(metadata float %0, i64 0, metadata !106, metadata !108), !dbg !105 ; var:"input" !DIExpression(DW_OP_bit_piece, 224, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %1, i64 0, metadata !106, metadata !109), !dbg !105 ; var:"input" !DIExpression(DW_OP_bit_piece, 256, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %25, i64 0, metadata !106, metadata !107), !dbg !105 ; var:"input" !DIExpression(DW_OP_bit_piece, 128, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %2, i64 0, metadata !106, metadata !110), !dbg !105 ; var:"input" !DIExpression(DW_OP_bit_piece, 160, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %26, i64 0, metadata !106, metadata !111), !dbg !105 ; var:"input" !DIExpression(DW_OP_bit_piece, 192, 32) func:"PSMain"
  %.i0 = fmul fast float %0, 8.000000e+00, !dbg !112 ; line:54 col:28
  %.i1 = fmul fast float %1, 8.000000e+00, !dbg !112 ; line:54 col:28
  call void @llvm.dbg.value(metadata float %.i0, i64 0, metadata !113, metadata !114), !dbg !115 ; var:"p" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float %.i1, i64 0, metadata !113, metadata !117), !dbg !115 ; var:"p" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float 0.000000e+00, i64 0, metadata !118, metadata !119), !dbg !120 ; var:"sum" !DIExpression() func:"Fbm"
  call void @llvm.dbg.value(metadata float 5.000000e-01, i64 0, metadata !121, metadata !119), !dbg !122 ; var:"amplitude" !DIExpression() func:"Fbm"
  call void @llvm.dbg.value(metadata i32 0, i64 0, metadata !123, metadata !119), !dbg !125 ; var:"i" !DIExpression() func:"Fbm"
  br label %3, !dbg !126 ; line:27 col:5

; <label>:3                                       ; preds = %3, %.lr.ph3
  %.0.i0 = phi float [ %.i0, %.lr.ph3 ], [ %.i015, %3 ]
  %.0.i1 = phi float [ %.i1, %.lr.ph3 ], [ %.i116, %3 ]
  %sum.i.0 = phi float [ 0.000000e+00, %.lr.ph3 ], [ %22, %3 ]
  %amplitude.i.0 = phi float [ 5.000000e-01, %.lr.ph3 ], [ %23, %3 ]
  %i.i.0 = phi i32 [ 0, %.lr.ph3 ], [ %24, %3 ]
  call void @llvm.dbg.value(metadata float %.0.i0, i64 0, metadata !113, metadata !114), !dbg !115 ; var:"p" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float %.0.i1, i64 0, metadata !113, metadata !117), !dbg !115 ; var:"p" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata i32 %i.i.0, i64 0, metadata !123, metadata !119), !dbg !125 ; var:"i" !DIExpression() func:"Fbm"
  call void @llvm.dbg.value(metadata float %amplitude.i.0, i64 0, metadata !121, metadata !119), !dbg !122 ; var:"amplitude" !DIExpression() func:"Fbm"
  call void @llvm.dbg.value(metadata float %sum.i.0, i64 0, metadata !118, metadata !119), !dbg !120 ; var:"sum" !DIExpression() func:"Fbm"
  %Round_ni = call float @dx.op.unary.f32(i32 27, float %.0.i0), !dbg !127 ; line:28 col:23  ; Round_ni(value)
  %Round_ni14 = call float @dx.op.unary.f32(i32 27, float %.0.i1), !dbg !127 ; line:28 col:23  ; Round_ni(value)
  call void @llvm.dbg.value(metadata float %Round_ni, i64 0, metadata !130, metadata !114), !dbg !131 ; var:"cell" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float %Round_ni14, i64 0, metadata !130, metadata !117), !dbg !131 ; var:"cell" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Fbm"
  %Frc12 = call float @dx.op.unary.f32(i32 22, float %.0.i0), !dbg !132 ; line:29 col:20  ; Frc(value)
  %Frc13 = call float @dx.op.unary.f32(i32 22, float %.0.i1), !dbg !132 ; line:29 col:20  ; Frc(value)
  call void @llvm.dbg.value(metadata float %Frc12, i64 0, metadata !133, metadata !114), !dbg !134 ; var:"f" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float %Frc13, i64 0, metadata !133, metadata !117), !dbg !134 ; var:"f" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float %Round_ni, i64 0, metadata !135, metadata !114), !dbg !136 ; var:"p" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Hash"
  call void @llvm.dbg.value(metadata float %Round_ni14, i64 0, metadata !135, metadata !117), !dbg !136 ; var:"p" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Hash"
  %4 = call float @dx.op.dot2.f32(i32 54, float %Round_ni, float %Round_ni14, float 0x4029FAC720000000, float 0x40538EE980000000), !dbg !138 ; line:20 col:21  ; Dot2(ax,ay,bx,by)
  %Sin10 = call float @dx.op.unary.f32(i32 13, float %4), !dbg !139 ; line:20 col:17  ; Sin(value)
  %5 = fmul fast float %Sin10, 0x40E55DD180000000, !dbg !140 ; line:20 col:54
  %Frc9 = call float @dx.op.unary.f32(i32 22, float %5), !dbg !141 ; line:20 col:12  ; Frc(value)
  call void @llvm.dbg.value(metadata float %Frc9, i64 0, metadata !142, metadata !119), !dbg !143 ; var:"a" !DIExpression() func:"Fbm"
  %.i017 = fadd fast float %Round_ni, 1.000000e+00, !dbg !144 ; line:31 col:29
  call void @llvm.dbg.value(metadata float %.i017, i64 0, metadata !135, metadata !114), !dbg !145 ; var:"p" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Hash"
  call void @llvm.dbg.value(metadata float %Round_ni14, i64 0, metadata !135, metadata !117), !dbg !145 ; var:"p" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Hash"
  %6 = call float @dx.op.dot2.f32(i32 54, float %.i017, float %Round_ni14, float 0x4029FAC720000000, float 0x40538EE980000000), !dbg !147 ; line:20 col:21  ; Dot2(ax,ay,bx,by)
  %Sin8 = call float @dx.op.unary.f32(i32 13, float %6), !dbg !148 ; line:20 col:17  ; Sin(value)
  %7 = fmul fast float %Sin8, 0x40E55DD180000000, !dbg !149 ; line:20 col:54
  %Frc7 = call float @dx.op.unary.f32(i32 22, float %7), !dbg !150 ; line:20 col:12  ; Frc(value)
  call void @llvm.dbg.value(metadata float %Frc7, i64 0, metadata !151, metadata !119), !dbg !152 ; var:"b" !DIExpression() func:"Fbm"
  %.i120 = fadd fast float %Round_ni14, 1.000000e+00, !dbg !153 ; line:32 col:29
  call void @llvm.dbg.value(metadata float %Round_ni, i64 0, metadata !135, metadata !114), !dbg !154 ; var:"p" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Hash"
  call void @llvm.dbg.value(metadata float %.i120, i64 0, metadata !135, metadata !117), !dbg !154 ; var:"p" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Hash"
  %8 = call float @dx.op.dot2.f32(i32 54, float %Round_ni, float %.i120, float 0x4029FAC720000000, float 0x40538EE980000000), !dbg !156 ; line:20 col:21  ; Dot2(ax,ay,bx,by)
  %Sin6 = call float @dx.op.unary.f32(i32 13, float %8), !dbg !157 ; line:20 col:17  ; Sin(value)
  %9 = fmul fast float %Sin6, 0x40E55DD180000000, !dbg !158 ; line:20 col:54
  %Frc5 = call float @dx.op.unary.f32(i32 22, float %9), !dbg !159 ; line:20 col:12  ; Frc(value)
  call void @llvm.dbg.value(metadata float %Frc5, i64 0, metadata !160, metadata !119), !dbg !161 ; var:"c" !DIExpression() func:"Fbm"
  call void @llvm.dbg.value(metadata float %.i017, i64 0, metadata !135, metadata !114), !dbg !162 ; var:"p" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Hash"
  call void @llvm.dbg.value(metadata float %.i120, i64 0, metadata !135, metadata !117), !dbg !162 ; var:"p" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Hash"
  %10 = call float @dx.op.dot2.f32(i32 54, float %.i017, float %.i120, float 0x4029FAC720000000, float 0x40538EE980000000), !dbg !164 ; line:20 col:21  ; Dot2(ax,ay,bx,by)
  %Sin = call float @dx.op.unary.f32(i32 13, float %10), !dbg !165 ; line:20 col:17  ; Sin(value)
  %11 = fmul fast float %Sin, 0x40E55DD180000000, !dbg !166 ; line:20 col:54
  %Frc = call float @dx.op.unary.f32(i32 22, float %11), !dbg !167 ; line:20 col:12  ; Frc(value)
  call void @llvm.dbg.value(metadata float %Frc, i64 0, metadata !168, metadata !119), !dbg !169 ; var:"d" !DIExpression() func:"Fbm"
  %.i023 = fmul fast float %Frc12, %Frc12, !dbg !170 ; line:34 col:22
  %.i124 = fmul fast float %Frc13, %Frc13, !dbg !170 ; line:34 col:22
  %.i025 = fmul fast float %Frc12, 2.000000e+00, !dbg !171 ; line:34 col:39
  %.i126 = fmul fast float %Frc13, 2.000000e+00, !dbg !171 ; line:34 col:39
  %.i027 = fsub fast float 3.000000e+00, %.i025, !dbg !172 ; line:34 col:33
  %.i128 = fsub fast float 3.000000e+00, %.i126, !dbg !172 ; line:34 col:33
  %.i029 = fmul fast float %.i023, %.i027, !dbg !173 ; line:34 col:26
  %.i130 = fmul fast float %.i124, %.i128, !dbg !173 ; line:34 col:26
  call void @llvm.dbg.value(metadata float %.i029, i64 0, metadata !174, metadata !114), !dbg !175 ; var:"u" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float %.i130, i64 0, metadata !174, metadata !117), !dbg !175 ; var:"u" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Fbm"
  %12 = fsub fast float %Frc, %Frc5, !dbg !176 ; line:35 col:50
  %13 = fmul fast float %12, %.i029, !dbg !176 ; line:35 col:50
  %14 = fsub fast float %Frc7, %Frc9, !dbg !177 ; line:35 col:33
  %15 = fmul fast float %14, %.i029, !dbg !177 ; line:35 col:33
  %16 = fadd fast float %15, %Frc9, !dbg !177 ; line:35 col:33
  %17 = fsub fast float %Frc5, %16, !dbg !176 ; line:35 col:50
  %18 = fadd fast float %17, %13, !dbg !178 ; line:35 col:28
  %19 = fmul fast float %.i130, %18, !dbg !178 ; line:35 col:28
  %20 = fadd fast float %19, %16, !dbg !178 ; line:35 col:28
  %21 = fmul fast float %20, %amplitude.i.0, !dbg !179 ; line:35 col:26
  %22 = fadd fast float %21, %sum.i.0, !dbg !180 ; line:35 col:13
  call void @llvm.dbg.value(metadata float %22, i64 0, metadata !118, metadata !119), !dbg !120 ; var:"sum" !DIExpression() func:"Fbm"
  %.i031 = fmul fast float %.0.i0, 0x40003D70A0000000, !dbg !181 ; line:36 col:15
  %.i132 = fmul fast float %.0.i1, 0x40003D70A0000000, !dbg !181 ; line:36 col:15
  %.i015 = fadd fast float %.i031, 0x3FC5C28F60000000, !dbg !182 ; line:36 col:22
  %.i116 = fadd fast float %.i132, 0x3FD3D70A40000000, !dbg !182 ; line:36 col:22
  call void @llvm.dbg.value(metadata float %.i015, i64 0, metadata !113, metadata !114), !dbg !115 ; var:"p" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Fbm"
  call void @llvm.dbg.value(metadata float %.i116, i64 0, metadata !113, metadata !117), !dbg !115 ; var:"p" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Fbm"
  %23 = fmul fast float %amplitude.i.0, 5.000000e-01, !dbg !183 ; line:37 col:19
  call void @llvm.dbg.value(metadata float %23, i64 0, metadata !121, metadata !119), !dbg !122 ; var:"amplitude" !DIExpression() func:"Fbm"
  %24 = add nuw nsw i32 %i.i.0, 1, !dbg !184 ; line:27 col:30
  call void @llvm.dbg.value(metadata i32 %24, i64 0, metadata !123, metadata !119), !dbg !125 ; var:"i" !DIExpression() func:"Fbm"
  %exitcond65 = icmp eq i32 %24, 480, !dbg !126 ; line:27 col:5
  br i1 %exitcond65, label %.lr.ph.preheader, label %3, !dbg !126 ; line:27 col:5

.lr.ph.preheader:                                 ; preds = %3
  %25 = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 0, i32 undef), !dbg !105 ; line:53 col:23  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  %26 = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 2, i32 undef), !dbg !105 ; line:53 col:23  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  br label %.lr.ph, !dbg !185 ; line:45 col:14

.lr.ph:                                           ; preds = %._crit_edge, %.lr.ph.preheader
  %y.i.0 = phi i32 [ %35, %._crit_edge ], [ 0, %.lr.ph.preheader ]
  %sum.i.1.0.i0 = phi float [ %.i035, %._crit_edge ], [ 0.000000e+00, %.lr.ph.preheader ]
  %sum.i.1.0.i1 = phi float [ %.i136, %._crit_edge ], [ 0.000000e+00, %.lr.ph.preheader ]
  %sum.i.1.0.i2 = phi float [ %.i2, %._crit_edge ], [ 0.000000e+00, %.lr.ph.preheader ]
  call void @llvm.dbg.value(metadata i32 %y.i.0, i64 0, metadata !186, metadata !119), !dbg !185 ; var:"y" !DIExpression() func:"Blurred"
  call void @llvm.dbg.value(metadata float %sum.i.1.0.i0, i64 0, metadata !187, metadata !114), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %sum.i.1.0.i1, i64 0, metadata !187, metadata !117), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %sum.i.1.0.i2, i64 0, metadata !187, metadata !189), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 64, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata i32 0, i64 0, metadata !190, metadata !119), !dbg !191 ; var:"x" !DIExpression() func:"Blurred"
  br label %27, !dbg !192 ; line:46 col:9

; <label>:27                                      ; preds = %27, %.lr.ph
  %x.i.0 = phi i32 [ 0, %.lr.ph ], [ %34, %27 ]
  %sum.i.1.1.i0 = phi float [ %sum.i.1.0.i0, %.lr.ph ], [ %.i035, %27 ]
  %sum.i.1.1.i1 = phi float [ %sum.i.1.0.i1, %.lr.ph ], [ %.i136, %27 ]
  %sum.i.1.1.i2 = phi float [ %sum.i.1.0.i2, %.lr.ph ], [ %.i2, %27 ]
  call void @llvm.dbg.value(metadata i32 %x.i.0, i64 0, metadata !190, metadata !119), !dbg !191 ; var:"x" !DIExpression() func:"Blurred"
  call void @llvm.dbg.value(metadata float %sum.i.1.1.i0, i64 0, metadata !187, metadata !114), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %sum.i.1.1.i1, i64 0, metadata !187, metadata !117), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %sum.i.1.1.i2, i64 0, metadata !187, metadata !189), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 64, 32) func:"Blurred"
  %28 = sitofp i32 %x.i.0 to float, !dbg !193 ; line:47 col:61
  %29 = sitofp i32 %y.i.0 to float, !dbg !194 ; line:47 col:64
  %.i037 = fmul fast float %28, 0x3F847AE140000000, !dbg !195 ; line:47 col:67
  %.i138 = fmul fast float %29, 0x3F847AE140000000, !dbg !195 ; line:47 col:67
  %.i039 = fadd fast float %.i037, %0, !dbg !196 ; line:47 col:52
  %.i140 = fadd fast float %.i138, %1, !dbg !196 ; line:47 col:52
  %30 = call %dx.types.ResRet.f32 @dx.op.sample.f32(i32 60, %dx.types.Handle %checker_texture_2d, %dx.types.Handle %pointSampler_sampler, float %.i039, float %.i140, float undef, float undef, i32 0, i32 0, i32 undef, float undef), !dbg !97 ; line:47 col:20  ; Sample(srv,sampler,coord0,coord1,coord2,coord3,offset0,offset1,offset2,clamp)
  %31 = extractvalue %dx.types.ResRet.f32 %30, 0, !dbg !97 ; line:47 col:20
  %32 = extractvalue %dx.types.ResRet.f32 %30, 1, !dbg !97 ; line:47 col:20
  %33 = extractvalue %dx.types.ResRet.f32 %30, 2, !dbg !97 ; line:47 col:20
  %.i035 = fadd fast float %31, %sum.i.1.1.i0, !dbg !197 ; line:47 col:17
  %.i136 = fadd fast float %32, %sum.i.1.1.i1, !dbg !197 ; line:47 col:17
  %.i2 = fadd fast float %33, %sum.i.1.1.i2, !dbg !197 ; line:47 col:17
  call void @llvm.dbg.value(metadata float %.i035, i64 0, metadata !187, metadata !114), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %.i136, i64 0, metadata !187, metadata !117), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %.i2, i64 0, metadata !187, metadata !189), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 64, 32) func:"Blurred"
  %34 = add nuw nsw i32 %x.i.0, 1, !dbg !198 ; line:46 col:32
  call void @llvm.dbg.value(metadata i32 %34, i64 0, metadata !190, metadata !119), !dbg !191 ; var:"x" !DIExpression() func:"Blurred"
  %exitcond = icmp eq i32 %34, 4, !dbg !192 ; line:46 col:9
  br i1 %exitcond, label %._crit_edge, label %27, !dbg !192 ; line:46 col:9

._crit_edge:                                      ; preds = %27
  call void @llvm.dbg.value(metadata float %.i035, i64 0, metadata !187, metadata !114), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %.i136, i64 0, metadata !187, metadata !117), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %.i2, i64 0, metadata !187, metadata !189), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 64, 32) func:"Blurred"
  %35 = add nuw nsw i32 %y.i.0, 1, !dbg !199 ; line:45 col:28
  call void @llvm.dbg.value(metadata i32 %35, i64 0, metadata !186, metadata !119), !dbg !185 ; var:"y" !DIExpression() func:"Blurred"
  %exitcond64 = icmp eq i32 %35, 4, !dbg !200 ; line:45 col:5
  br i1 %exitcond64, label %"\01?Blurred@@YA?AV?$vector@M$02@@V?$vector@M$01@@@Z.exit", label %.lr.ph, !dbg !200 ; line:45 col:5

"\01?Blurred@@YA?AV?$vector@M$02@@V?$vector@M$01@@@Z.exit": ; preds = %._crit_edge
  call void @llvm.dbg.value(metadata float %.i035, i64 0, metadata !187, metadata !114), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 0, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %.i136, i64 0, metadata !187, metadata !117), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 32, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %.i2, i64 0, metadata !187, metadata !189), !dbg !188 ; var:"sum" !DIExpression(DW_OP_bit_piece, 64, 32) func:"Blurred"
  call void @llvm.dbg.value(metadata float %.i044, i64 0, metadata !201, metadata !114), !dbg !202 ; var:"tex" !DIExpression(DW_OP_bit_piece, 0, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %.i044, i64 0, metadata !201, metadata !117), !dbg !202 ; var:"tex" !DIExpression(DW_OP_bit_piece, 32, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %.i044, i64 0, metadata !201, metadata !189), !dbg !202 ; var:"tex" !DIExpression(DW_OP_bit_piece, 64, 32) func:"PSMain"
  %36 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %Frame_cbuffer, i32 0), !dbg !203 ; line:56 col:37  ; CBufferLoadLegacy(handle,regIndex)
  %37 = extractvalue %dx.types.CBufRet.f32 %36, 0, !dbg !203 ; line:56 col:37
  %38 = fmul fast float %37, 2.000000e+00, !dbg !204 ; line:56 col:42
  %Sin11 = call float @dx.op.unary.f32(i32 13, float %38), !dbg !205 ; line:56 col:33  ; Sin(value)
  %39 = fmul fast float %Sin11, 2.500000e-01, !dbg !206 ; line:56 col:31
  %40 = fadd fast float %39, 7.500000e-01, !dbg !207 ; line:56 col:24
  call void @llvm.dbg.value(metadata float %40, i64 0, metadata !208, metadata !119), !dbg !209 ; var:"pulse" !DIExpression() func:"PSMain"
  %41 = fadd fast float %22, 5.000000e-01, !dbg !210 ; line:57 col:49
  %.i044 = fmul fast float %41, 6.250000e-02, !dbg !211 ; line:50 col:16
  %.i047 = fmul fast float %.i035, %.i044, !dbg !212 ; line:57 col:42
  %.i148 = fmul fast float %.i136, %.i044, !dbg !212 ; line:57 col:42
  %.i249 = fmul fast float %.i2, %.i044, !dbg !212 ; line:57 col:42
  %.i050 = fsub fast float %.i047, %25, !dbg !213 ; line:57 col:20
  %.i151 = fsub fast float %.i148, %2, !dbg !213 ; line:57 col:20
  %.i252 = fsub fast float %.i249, %26, !dbg !213 ; line:57 col:20
  %.i053 = fmul fast float %40, %.i050, !dbg !213 ; line:57 col:20
  %.i154 = fmul fast float %40, %.i151, !dbg !213 ; line:57 col:20
  %.i255 = fmul fast float %40, %.i252, !dbg !213 ; line:57 col:20
  %.i056 = fadd fast float %.i053, %25, !dbg !213 ; line:57 col:20
  %.i157 = fadd fast float %.i154, %2, !dbg !213 ; line:57 col:20
  %.i258 = fadd fast float %.i255, %26, !dbg !213 ; line:57 col:20
  call void @llvm.dbg.value(metadata float %.i056, i64 0, metadata !214, metadata !114), !dbg !215 ; var:"color" !DIExpression(DW_OP_bit_piece, 0, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %.i157, i64 0, metadata !214, metadata !117), !dbg !215 ; var:"color" !DIExpression(DW_OP_bit_piece, 32, 32) func:"PSMain"
  call void @llvm.dbg.value(metadata float %.i258, i64 0, metadata !214, metadata !189), !dbg !215 ; var:"color" !DIExpression(DW_OP_bit_piece, 64, 32) func:"PSMain"
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %.i056), !dbg !216 ; line:58 col:5  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %.i157), !dbg !216 ; line:58 col:5  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %.i258), !dbg !216 ; line:58 col:5  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float 1.000000e+00), !dbg !216 ; line:58 col:5  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  ret void, !dbg !216 ; line:58 col:5
}

; Function Attrs: nounwind readnone
declare void @llvm.dbg.value(metadata, i64, metadata, metadata) #0

; Function Attrs: nounwind readnone
declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) #0

; Function Attrs: nounwind
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) #1

; Function Attrs: nounwind readnone
declare float @dx.op.unary.f32(i32, float) #0

; Function Attrs: nounwind readnone
declare float @dx.op.dot2.f32(i32, float, float, float, float) #0

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.f32 @dx.op.sample.f32(i32, %dx.types.Handle, %dx.types.Handle, float, float, float, float, i32, i32, i32, float) #2

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind }
attributes #2 = { nounwind readonly }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!60, !61}
!llvm.ident = !{!62}
!dx.source.contents = !{!63}
!dx.source.defines = !{!2}
!dx.source.mainFileName = !{!64}
!dx.source.args = !{!65}
!dx.version = !{!66}
!dx.valver = !{!67}
!dx.shaderModel = !{!68}
!dx.resources = !{!69}
!dx.typeAnnotations = !{!77, !81}
!dx.viewIdState = !{!84}
!dx.entryPoints = !{!85}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: "dxc(private) 1.8.0.4775 (d39324e06)", isOptimized: false, runtimeVersion: 0, emissionKind: 1, enums: !2, retainedTypes: !3, subprograms: !23, globals: !47)
!1 = !DIFile(filename: "D:/src/gpu_inspector/test/d3d12_triangle/heavy.hlsl", directory: "")
!2 = !{}
!3 = !{!4, !16}
!4 = !DIDerivedType(tag: DW_TAG_typedef, name: "float4", file: !1, line: 7, baseType: !5)
!5 = !DICompositeType(tag: DW_TAG_class_type, name: "vector<float, 4>", file: !1, line: 7, size: 128, align: 32, elements: !6, templateParams: !12)
!6 = !{!7, !9, !10, !11}
!7 = !DIDerivedType(tag: DW_TAG_member, name: "x", scope: !5, file: !1, line: 7, baseType: !8, size: 32, align: 32, flags: DIFlagPublic)
!8 = !DIBasicType(name: "float", size: 32, align: 32, encoding: DW_ATE_float)
!9 = !DIDerivedType(tag: DW_TAG_member, name: "y", scope: !5, file: !1, line: 7, baseType: !8, size: 32, align: 32, offset: 32, flags: DIFlagPublic)
!10 = !DIDerivedType(tag: DW_TAG_member, name: "z", scope: !5, file: !1, line: 7, baseType: !8, size: 32, align: 32, offset: 64, flags: DIFlagPublic)
!11 = !DIDerivedType(tag: DW_TAG_member, name: "w", scope: !5, file: !1, line: 7, baseType: !8, size: 32, align: 32, offset: 96, flags: DIFlagPublic)
!12 = !{!13, !14}
!13 = !DITemplateTypeParameter(name: "element", type: !8)
!14 = !DITemplateValueParameter(name: "element_count", type: !15, value: i32 4)
!15 = !DIBasicType(name: "int", size: 32, align: 32, encoding: DW_ATE_signed)
!16 = !DIDerivedType(tag: DW_TAG_typedef, name: "float2", file: !1, line: 7, baseType: !17)
!17 = !DICompositeType(tag: DW_TAG_class_type, name: "vector<float, 2>", file: !1, line: 7, size: 64, align: 32, elements: !18, templateParams: !21)
!18 = !{!19, !20}
!19 = !DIDerivedType(tag: DW_TAG_member, name: "x", scope: !17, file: !1, line: 7, baseType: !8, size: 32, align: 32, flags: DIFlagPublic)
!20 = !DIDerivedType(tag: DW_TAG_member, name: "y", scope: !17, file: !1, line: 7, baseType: !8, size: 32, align: 32, offset: 32, flags: DIFlagPublic)
!21 = !{!13, !22}
!22 = !DITemplateValueParameter(name: "element_count", type: !15, value: i32 2)
!23 = !{!24, !40, !43, !44}
!24 = !DISubprogram(name: "PSMain", scope: !1, file: !1, line: 53, type: !25, isLocal: false, isDefinition: true, scopeLine: 53, flags: DIFlagPrototyped, isOptimized: false, function: void ()* @PSMain)
!25 = !DISubroutineType(types: !26)
!26 = !{!4, !27}
!27 = !DICompositeType(tag: DW_TAG_structure_type, name: "PSInput", file: !1, line: 13, size: 288, align: 32, elements: !28)
!28 = !{!29, !30, !39}
!29 = !DIDerivedType(tag: DW_TAG_member, name: "position", scope: !27, file: !1, line: 14, baseType: !4, size: 128, align: 32)
!30 = !DIDerivedType(tag: DW_TAG_member, name: "color", scope: !27, file: !1, line: 15, baseType: !31, size: 96, align: 32, offset: 128)
!31 = !DIDerivedType(tag: DW_TAG_typedef, name: "float3", file: !1, line: 7, baseType: !32)
!32 = !DICompositeType(tag: DW_TAG_class_type, name: "vector<float, 3>", file: !1, line: 7, size: 96, align: 32, elements: !33, templateParams: !37)
!33 = !{!34, !35, !36}
!34 = !DIDerivedType(tag: DW_TAG_member, name: "x", scope: !32, file: !1, line: 7, baseType: !8, size: 32, align: 32, flags: DIFlagPublic)
!35 = !DIDerivedType(tag: DW_TAG_member, name: "y", scope: !32, file: !1, line: 7, baseType: !8, size: 32, align: 32, offset: 32, flags: DIFlagPublic)
!36 = !DIDerivedType(tag: DW_TAG_member, name: "z", scope: !32, file: !1, line: 7, baseType: !8, size: 32, align: 32, offset: 64, flags: DIFlagPublic)
!37 = !{!13, !38}
!38 = !DITemplateValueParameter(name: "element_count", type: !15, value: i32 3)
!39 = !DIDerivedType(tag: DW_TAG_member, name: "uv", scope: !27, file: !1, line: 16, baseType: !16, size: 64, align: 32, offset: 224)
!40 = !DISubprogram(name: "Fbm", linkageName: "\01?Fbm@@YAMV?$vector@M$01@@@Z", scope: !1, file: !1, line: 24, type: !41, isLocal: false, isDefinition: true, scopeLine: 24, flags: DIFlagPrototyped, isOptimized: false)
!41 = !DISubroutineType(types: !42)
!42 = !{!8, !16}
!43 = !DISubprogram(name: "Hash", linkageName: "\01?Hash@@YAMV?$vector@M$01@@@Z", scope: !1, file: !1, line: 19, type: !41, isLocal: false, isDefinition: true, scopeLine: 19, flags: DIFlagPrototyped, isOptimized: false)
!44 = !DISubprogram(name: "Blurred", linkageName: "\01?Blurred@@YA?AV?$vector@M$02@@V?$vector@M$01@@@Z", scope: !1, file: !1, line: 43, type: !45, isLocal: false, isDefinition: true, scopeLine: 43, flags: DIFlagPrototyped, isOptimized: false)
!45 = !DISubroutineType(types: !46)
!46 = !{!31, !16}
!47 = !{!48, !50, !54, !58}
!48 = !DIGlobalVariable(name: "time", linkageName: "\01?time@Frame@@3MB", scope: !0, file: !1, line: 6, type: !49, isLocal: false, isDefinition: true)
!49 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !8)
!50 = !DIGlobalVariable(name: "flags", linkageName: "\01?flags@Frame@@3IB", scope: !0, file: !1, line: 7, type: !51, isLocal: false, isDefinition: true)
!51 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !52)
!52 = !DIDerivedType(tag: DW_TAG_typedef, name: "uint", file: !1, line: 7, baseType: !53)
!53 = !DIBasicType(name: "unsigned int", size: 32, align: 32, encoding: DW_ATE_unsigned)
!54 = !DIGlobalVariable(name: "checker", linkageName: "\01?checker@@3V?$Texture2D@V?$vector@M$03@@@@A", scope: !0, file: !1, line: 10, type: !55, isLocal: false, isDefinition: true)
!55 = !DICompositeType(tag: DW_TAG_class_type, name: "Texture2D<vector<float, 4> >", file: !1, line: 10, size: 160, align: 32, elements: !2, templateParams: !56)
!56 = !{!57}
!57 = !DITemplateTypeParameter(name: "element", type: !5)
!58 = !DIGlobalVariable(name: "pointSampler", linkageName: "\01?pointSampler@@3USamplerState@@A", scope: !0, file: !1, line: 11, type: !59, isLocal: false, isDefinition: true)
!59 = !DICompositeType(tag: DW_TAG_structure_type, name: "SamplerState", file: !1, line: 11, size: 32, align: 32, elements: !2)
!60 = !{i32 2, !"Dwarf Version", i32 4}
!61 = !{i32 2, !"Debug Info Version", i32 3}
!62 = !{!"dxc(private) 1.8.0.4775 (d39324e06)"}
!63 = !{!"D:\5Csrc\5Cgpu_inspector\5Ctest\5Cd3d12_triangle\5Cheavy.hlsl", !"// --heavy: the cube's pixel shader with work of a known relative cost in named functions, for the\0A// Shader Flame Graph's measurements by ablation (the counterpart of test/triangle/heavy.frag).\0A// Fbm() does far more than Blurred(), and PSMain() itself almost nothing. Bound as cube.hlsl is.\0A\0Acbuffer Frame : register(b1) {\0A    float time;\0A    uint flags;\0A};\0A\0ATexture2D checker : register(t0);\0ASamplerState pointSampler : register(s0);\0A\0Astruct PSInput {\0A    float4 position : SV_Position;\0A    float3 color : COLOR;\0A    float2 uv : TEXCOORD;\0A};\0A\0Afloat Hash(float2 p) {\0A    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);\0A}\0A\0A// Value noise summed over octaves: four hashes an octave.\0Afloat Fbm(float2 p) {\0A    float sum = 0.0;\0A    float amplitude = 0.5;\0A    for (int i = 0; i < 480; ++i) {\0A        float2 cell = floor(p);\0A        float2 f = frac(p);\0A        float a = Hash(cell);\0A        float b = Hash(cell + float2(1.0, 0.0));\0A        float c = Hash(cell + float2(0.0, 1.0));\0A        float d = Hash(cell + float2(1.0, 1.0));\0A        float2 u = f * f * (3.0 - 2.0 * f);\0A        sum += amplitude * lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);\0A        p = p * 2.03 + float2(0.17, 0.31);\0A        amplitude *= 0.5;\0A    }\0A    return sum;\0A}\0A\0A// A box blur of the checker texture: sixteen samples.\0Afloat3 Blurred(float2 uv) {\0A    float3 sum = 0.0;\0A    for (int y = 0; y < 4; ++y) {\0A        for (int x = 0; x < 4; ++x) {\0A            sum += checker.Sample(pointSampler, uv + float2(x, y) * 0.01).rgb;\0A        }\0A    }\0A    return sum / 16.0;\0A}\0A\0Afloat4 PSMain(PSInput input) : SV_Target {\0A    float n = Fbm(input.uv * 8.0);\0A    float3 tex = Blurred(input.uv);\0A    float pulse = 0.75 + 0.25 * sin(time * 2.0);\0A    float3 color = lerp(input.color, tex * (0.5 + n), pulse);\0A    return float4(color, 1.0);\0A}\0A"}
!64 = !{!"D:\5Csrc\5Cgpu_inspector\5Ctest\5Cd3d12_triangle\5Cheavy.hlsl"}
!65 = !{!"-E", !"PSMain", !"-T", !"ps_6_0", !"-Zi", !"-Qembed_debug", !"-Fo", !"D:/src/gpu_inspector/build/test/d3d12_triangle/heavy_ps.cso"}
!66 = !{i32 1, i32 0}
!67 = !{i32 1, i32 8}
!68 = !{!"ps", i32 6, i32 0}
!69 = !{!70, null, !73, !75}
!70 = !{!71}
!71 = !{i32 0, %"class.Texture2D<vector<float, 4> >"* undef, !"checker", i32 0, i32 0, i32 1, i32 2, i32 0, !72}
!72 = !{i32 0, i32 9}
!73 = !{!74}
!74 = !{i32 0, %Frame* undef, !"Frame", i32 0, i32 1, i32 1, i32 8, null}
!75 = !{!76}
!76 = !{i32 0, %struct.SamplerState* undef, !"pointSampler", i32 0, i32 0, i32 1, i32 0, null}
!77 = !{i32 0, %Frame undef, !78}
!78 = !{i32 8, !79, !80}
!79 = !{i32 6, !"time", i32 3, i32 0, i32 7, i32 9}
!80 = !{i32 6, !"flags", i32 3, i32 4, i32 7, i32 5}
!81 = !{i32 1, void ()* @PSMain, !82}
!82 = !{!83}
!83 = !{i32 0, !2, !2}
!84 = !{[12 x i32] [i32 10, i32 4, i32 0, i32 0, i32 0, i32 0, i32 1, i32 2, i32 4, i32 0, i32 7, i32 7]}
!85 = !{void ()* @PSMain, !"PSMain", !86, !69, null}
!86 = !{!87, !94, null}
!87 = !{!88, !90, !92}
!88 = !{i32 0, !"SV_Position", i8 9, i8 3, !89, i8 4, i32 1, i8 4, i32 0, i8 0, null}
!89 = !{i32 0}
!90 = !{i32 1, !"COLOR", i8 9, i8 0, !89, i8 2, i32 1, i8 3, i32 1, i8 0, !91}
!91 = !{i32 3, i32 7}
!92 = !{i32 2, !"TEXCOORD", i8 9, i8 0, !89, i8 2, i32 1, i8 2, i32 2, i8 0, !93}
!93 = !{i32 3, i32 3}
!94 = !{!95}
!95 = !{i32 0, !"SV_Target", i8 9, i8 16, !89, i8 0, i32 1, i8 4, i32 0, i8 0, !96}
!96 = !{i32 3, i32 15}
!97 = !DILocation(line: 47, column: 20, scope: !98, inlinedAt: !104)
!98 = distinct !DILexicalBlock(scope: !99, file: !1, line: 46, column: 37)
!99 = distinct !DILexicalBlock(scope: !100, file: !1, line: 46, column: 9)
!100 = distinct !DILexicalBlock(scope: !101, file: !1, line: 46, column: 9)
!101 = distinct !DILexicalBlock(scope: !102, file: !1, line: 45, column: 33)
!102 = distinct !DILexicalBlock(scope: !103, file: !1, line: 45, column: 5)
!103 = distinct !DILexicalBlock(scope: !44, file: !1, line: 45, column: 5)
!104 = distinct !DILocation(line: 55, column: 18, scope: !24)
!105 = !DILocation(line: 53, column: 23, scope: !24)
!106 = !DILocalVariable(tag: DW_TAG_arg_variable, name: "input", arg: 1, scope: !24, file: !1, line: 53, type: !27)
!107 = !DIExpression(DW_OP_bit_piece, 128, 32)
!108 = !DIExpression(DW_OP_bit_piece, 224, 32)
!109 = !DIExpression(DW_OP_bit_piece, 256, 32)
!110 = !DIExpression(DW_OP_bit_piece, 160, 32)
!111 = !DIExpression(DW_OP_bit_piece, 192, 32)
!112 = !DILocation(line: 54, column: 28, scope: !24)
!113 = !DILocalVariable(tag: DW_TAG_arg_variable, name: "p", arg: 1, scope: !40, file: !1, line: 24, type: !16)
!114 = !DIExpression(DW_OP_bit_piece, 0, 32)
!115 = !DILocation(line: 24, column: 18, scope: !40, inlinedAt: !116)
!116 = distinct !DILocation(line: 54, column: 15, scope: !24)
!117 = !DIExpression(DW_OP_bit_piece, 32, 32)
!118 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "sum", scope: !40, file: !1, line: 25, type: !8)
!119 = !DIExpression()
!120 = !DILocation(line: 25, column: 11, scope: !40, inlinedAt: !116)
!121 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "amplitude", scope: !40, file: !1, line: 26, type: !8)
!122 = !DILocation(line: 26, column: 11, scope: !40, inlinedAt: !116)
!123 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "i", scope: !124, file: !1, line: 27, type: !15)
!124 = distinct !DILexicalBlock(scope: !40, file: !1, line: 27, column: 5)
!125 = !DILocation(line: 27, column: 14, scope: !124, inlinedAt: !116)
!126 = !DILocation(line: 27, column: 5, scope: !124, inlinedAt: !116)
!127 = !DILocation(line: 28, column: 23, scope: !128, inlinedAt: !116)
!128 = distinct !DILexicalBlock(scope: !129, file: !1, line: 27, column: 35)
!129 = distinct !DILexicalBlock(scope: !124, file: !1, line: 27, column: 5)
!130 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "cell", scope: !128, file: !1, line: 28, type: !16)
!131 = !DILocation(line: 28, column: 16, scope: !128, inlinedAt: !116)
!132 = !DILocation(line: 29, column: 20, scope: !128, inlinedAt: !116)
!133 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "f", scope: !128, file: !1, line: 29, type: !16)
!134 = !DILocation(line: 29, column: 16, scope: !128, inlinedAt: !116)
!135 = !DILocalVariable(tag: DW_TAG_arg_variable, name: "p", arg: 1, scope: !43, file: !1, line: 19, type: !16)
!136 = !DILocation(line: 19, column: 19, scope: !43, inlinedAt: !137)
!137 = distinct !DILocation(line: 30, column: 19, scope: !128, inlinedAt: !116)
!138 = !DILocation(line: 20, column: 21, scope: !43, inlinedAt: !137)
!139 = !DILocation(line: 20, column: 17, scope: !43, inlinedAt: !137)
!140 = !DILocation(line: 20, column: 54, scope: !43, inlinedAt: !137)
!141 = !DILocation(line: 20, column: 12, scope: !43, inlinedAt: !137)
!142 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "a", scope: !128, file: !1, line: 30, type: !8)
!143 = !DILocation(line: 30, column: 15, scope: !128, inlinedAt: !116)
!144 = !DILocation(line: 31, column: 29, scope: !128, inlinedAt: !116)
!145 = !DILocation(line: 19, column: 19, scope: !43, inlinedAt: !146)
!146 = distinct !DILocation(line: 31, column: 19, scope: !128, inlinedAt: !116)
!147 = !DILocation(line: 20, column: 21, scope: !43, inlinedAt: !146)
!148 = !DILocation(line: 20, column: 17, scope: !43, inlinedAt: !146)
!149 = !DILocation(line: 20, column: 54, scope: !43, inlinedAt: !146)
!150 = !DILocation(line: 20, column: 12, scope: !43, inlinedAt: !146)
!151 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "b", scope: !128, file: !1, line: 31, type: !8)
!152 = !DILocation(line: 31, column: 15, scope: !128, inlinedAt: !116)
!153 = !DILocation(line: 32, column: 29, scope: !128, inlinedAt: !116)
!154 = !DILocation(line: 19, column: 19, scope: !43, inlinedAt: !155)
!155 = distinct !DILocation(line: 32, column: 19, scope: !128, inlinedAt: !116)
!156 = !DILocation(line: 20, column: 21, scope: !43, inlinedAt: !155)
!157 = !DILocation(line: 20, column: 17, scope: !43, inlinedAt: !155)
!158 = !DILocation(line: 20, column: 54, scope: !43, inlinedAt: !155)
!159 = !DILocation(line: 20, column: 12, scope: !43, inlinedAt: !155)
!160 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "c", scope: !128, file: !1, line: 32, type: !8)
!161 = !DILocation(line: 32, column: 15, scope: !128, inlinedAt: !116)
!162 = !DILocation(line: 19, column: 19, scope: !43, inlinedAt: !163)
!163 = distinct !DILocation(line: 33, column: 19, scope: !128, inlinedAt: !116)
!164 = !DILocation(line: 20, column: 21, scope: !43, inlinedAt: !163)
!165 = !DILocation(line: 20, column: 17, scope: !43, inlinedAt: !163)
!166 = !DILocation(line: 20, column: 54, scope: !43, inlinedAt: !163)
!167 = !DILocation(line: 20, column: 12, scope: !43, inlinedAt: !163)
!168 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "d", scope: !128, file: !1, line: 33, type: !8)
!169 = !DILocation(line: 33, column: 15, scope: !128, inlinedAt: !116)
!170 = !DILocation(line: 34, column: 22, scope: !128, inlinedAt: !116)
!171 = !DILocation(line: 34, column: 39, scope: !128, inlinedAt: !116)
!172 = !DILocation(line: 34, column: 33, scope: !128, inlinedAt: !116)
!173 = !DILocation(line: 34, column: 26, scope: !128, inlinedAt: !116)
!174 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "u", scope: !128, file: !1, line: 34, type: !16)
!175 = !DILocation(line: 34, column: 16, scope: !128, inlinedAt: !116)
!176 = !DILocation(line: 35, column: 50, scope: !128, inlinedAt: !116)
!177 = !DILocation(line: 35, column: 33, scope: !128, inlinedAt: !116)
!178 = !DILocation(line: 35, column: 28, scope: !128, inlinedAt: !116)
!179 = !DILocation(line: 35, column: 26, scope: !128, inlinedAt: !116)
!180 = !DILocation(line: 35, column: 13, scope: !128, inlinedAt: !116)
!181 = !DILocation(line: 36, column: 15, scope: !128, inlinedAt: !116)
!182 = !DILocation(line: 36, column: 22, scope: !128, inlinedAt: !116)
!183 = !DILocation(line: 37, column: 19, scope: !128, inlinedAt: !116)
!184 = !DILocation(line: 27, column: 30, scope: !129, inlinedAt: !116)
!185 = !DILocation(line: 45, column: 14, scope: !103, inlinedAt: !104)
!186 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "y", scope: !103, file: !1, line: 45, type: !15)
!187 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "sum", scope: !44, file: !1, line: 44, type: !31)
!188 = !DILocation(line: 44, column: 12, scope: !44, inlinedAt: !104)
!189 = !DIExpression(DW_OP_bit_piece, 64, 32)
!190 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "x", scope: !100, file: !1, line: 46, type: !15)
!191 = !DILocation(line: 46, column: 18, scope: !100, inlinedAt: !104)
!192 = !DILocation(line: 46, column: 9, scope: !100, inlinedAt: !104)
!193 = !DILocation(line: 47, column: 61, scope: !98, inlinedAt: !104)
!194 = !DILocation(line: 47, column: 64, scope: !98, inlinedAt: !104)
!195 = !DILocation(line: 47, column: 67, scope: !98, inlinedAt: !104)
!196 = !DILocation(line: 47, column: 52, scope: !98, inlinedAt: !104)
!197 = !DILocation(line: 47, column: 17, scope: !98, inlinedAt: !104)
!198 = !DILocation(line: 46, column: 32, scope: !99, inlinedAt: !104)
!199 = !DILocation(line: 45, column: 28, scope: !102, inlinedAt: !104)
!200 = !DILocation(line: 45, column: 5, scope: !103, inlinedAt: !104)
!201 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "tex", scope: !24, file: !1, line: 55, type: !31)
!202 = !DILocation(line: 55, column: 12, scope: !24)
!203 = !DILocation(line: 56, column: 37, scope: !24)
!204 = !DILocation(line: 56, column: 42, scope: !24)
!205 = !DILocation(line: 56, column: 33, scope: !24)
!206 = !DILocation(line: 56, column: 31, scope: !24)
!207 = !DILocation(line: 56, column: 24, scope: !24)
!208 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "pulse", scope: !24, file: !1, line: 56, type: !8)
!209 = !DILocation(line: 56, column: 11, scope: !24)
!210 = !DILocation(line: 57, column: 49, scope: !24)
!211 = !DILocation(line: 50, column: 16, scope: !44, inlinedAt: !104)
!212 = !DILocation(line: 57, column: 42, scope: !24)
!213 = !DILocation(line: 57, column: 20, scope: !24)
!214 = !DILocalVariable(tag: DW_TAG_auto_variable, name: "color", scope: !24, file: !1, line: 57, type: !31)
!215 = !DILocation(line: 57, column: 12, scope: !24)
!216 = !DILocation(line: 58, column: 5, scope: !24)
