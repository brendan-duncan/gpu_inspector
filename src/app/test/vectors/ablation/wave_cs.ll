;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
; shader debug name: dd68999e3872820c1266eed57a465869.pdb
; shader hash: dd68999e3872820c1266eed57a465869
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(64,1,1)
; MinimumExpectedWaveLaneCount: 0
; MaximumExpectedWaveLaneCount: 4294967295
; UsesViewID: false
; SigInputElements: 0
; SigOutputElements: 0
; SigPatchConstOrPrimElements: 0
; SigInputVectors: 0
; SigOutputVectors[0]: 0
; SigOutputVectors[1]: 0
; SigOutputVectors[2]: 0
; SigOutputVectors[3]: 0
; EntryFunctionName: CSMain
;
;
; Buffer Definitions:
;
; cbuffer Params
; {
;
;   struct Params
;   {
;
;       float time;                                   ; Offset:    0
;       uint count;                                   ; Offset:    4
;   
;   } Params;                                         ; Offset:    0 Size:     8
;
; }
;
; Resource bind info for wave
; {
;
;   float $Element;                                   ; Offset:    0 Size:     4
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; Params                            cbuffer      NA          NA     CB0            cb0     1
; wave                                  UAV  struct         r/w      U0             u0     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%"class.RWStructuredBuffer<float>" = type { float }
%Params = type { float, i32 }

define void @CSMain() {
  %wave_UAV_structbuf = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %Params_cbuffer = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %1 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  call void @llvm.dbg.value(metadata i32 %1, i64 0, metadata !56, metadata !57), !dbg !58 ; var:"id" !DIExpression(DW_OP_bit_piece, 0, 32) func:"CSMain"
  %2 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %Params_cbuffer, i32 0), !dbg !59 ; line:15 col:16  ; CBufferLoadLegacy(handle,regIndex)
  %3 = extractvalue %dx.types.CBufRet.i32 %2, 1, !dbg !59 ; line:15 col:16
  %4 = icmp ult i32 %1, %3, !dbg !61 ; line:15 col:14
  br i1 %4, label %5, label %11, !dbg !62 ; line:15 col:9

; <label>:5                                       ; preds = %0
  %6 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %Params_cbuffer, i32 0), !dbg !63 ; line:15 col:40  ; CBufferLoadLegacy(handle,regIndex)
  %7 = extractvalue %dx.types.CBufRet.f32 %6, 0, !dbg !63 ; line:15 col:40
  %8 = uitofp i32 %1 to float, !dbg !64 ; line:15 col:47
  %9 = fmul fast float %8, 0x3FA99999A0000000, !dbg !65 ; line:15 col:52
  %10 = fadd fast float %7, %9, !dbg !66 ; line:15 col:45
  %Sin = call float @dx.op.unary.f32(i32 13, float %10), !dbg !67 ; line:15 col:36  ; Sin(value)
  call void @dx.op.bufferStore.f32(i32 69, %dx.types.Handle %wave_UAV_structbuf, i32 %1, i32 0, float %Sin, float undef, float undef, float undef, i8 1), !dbg !68 ; line:15 col:34  ; BufferStore(uav,coord0,coord1,value0,value1,value2,value3,mask)
  br label %11, !dbg !69 ; line:15 col:23

; <label>:11                                      ; preds = %5, %0
  ret void, !dbg !70 ; line:16 col:1
}

; Function Attrs: nounwind readnone
declare void @llvm.dbg.value(metadata, i64, metadata, metadata) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readnone
declare float @dx.op.unary.f32(i32, float) #0

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #1

; Function Attrs: nounwind
declare void @dx.op.bufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, float, float, i8) #2

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind readonly }
attributes #2 = { nounwind }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!29, !30}
!llvm.ident = !{!31}
!dx.source.contents = !{!32}
!dx.source.defines = !{!2}
!dx.source.mainFileName = !{!33}
!dx.source.args = !{!34}
!dx.version = !{!35}
!dx.valver = !{!36}
!dx.shaderModel = !{!37}
!dx.resources = !{!38}
!dx.typeAnnotations = !{!44, !50}
!dx.entryPoints = !{!53}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: "dxc(private) 1.8.0.4775 (d39324e06)", isOptimized: false, runtimeVersion: 0, emissionKind: 1, enums: !2, subprograms: !3, globals: !18)
!1 = !DIFile(filename: "D:/src/gpu_inspector/test/d3d12_triangle/wave.hlsl", directory: "")
!2 = !{}
!3 = !{!4}
!4 = !DISubprogram(name: "CSMain", scope: !1, file: !1, line: 14, type: !5, isLocal: false, isDefinition: true, scopeLine: 14, flags: DIFlagPrototyped, isOptimized: false, function: void ()* @CSMain)
!5 = !DISubroutineType(types: !6)
!6 = !{null, !7}
!7 = !DIDerivedType(tag: DW_TAG_typedef, name: "uint3", file: !1, line: 7, baseType: !8)
!8 = !DICompositeType(tag: DW_TAG_class_type, name: "vector<unsigned int, 3>", file: !1, line: 7, size: 96, align: 32, elements: !9, templateParams: !14)
!9 = !{!10, !12, !13}
!10 = !DIDerivedType(tag: DW_TAG_member, name: "x", scope: !8, file: !1, line: 7, baseType: !11, size: 32, align: 32, flags: DIFlagPublic)
!11 = !DIBasicType(name: "unsigned int", size: 32, align: 32, encoding: DW_ATE_unsigned)
!12 = !DIDerivedType(tag: DW_TAG_member, name: "y", scope: !8, file: !1, line: 7, baseType: !11, size: 32, align: 32, offset: 32, flags: DIFlagPublic)
!13 = !DIDerivedType(tag: DW_TAG_member, name: "z", scope: !8, file: !1, line: 7, baseType: !11, size: 32, align: 32, offset: 64, flags: DIFlagPublic)
!14 = !{!15, !16}
!15 = !DITemplateTypeParameter(name: "element", type: !11)
!16 = !DITemplateValueParameter(name: "element_count", type: !17, value: i32 3)
!17 = !DIBasicType(name: "int", size: 32, align: 32, encoding: DW_ATE_signed)
!18 = !{!19, !22, !25}
!19 = !DIGlobalVariable(name: "time", linkageName: "\01?time@Params@@3MB", scope: !0, file: !1, line: 6, type: !20, isLocal: false, isDefinition: true)
!20 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !21)
!21 = !DIBasicType(name: "float", size: 32, align: 32, encoding: DW_ATE_float)
!22 = !DIGlobalVariable(name: "count", linkageName: "\01?count@Params@@3IB", scope: !0, file: !1, line: 7, type: !23, isLocal: false, isDefinition: true)
!23 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !24)
!24 = !DIDerivedType(tag: DW_TAG_typedef, name: "uint", file: !1, line: 7, baseType: !11)
!25 = !DIGlobalVariable(name: "wave", linkageName: "\01?wave@@3V?$RWStructuredBuffer@M@@A", scope: !0, file: !1, line: 11, type: !26, isLocal: false, isDefinition: true)
!26 = !DICompositeType(tag: DW_TAG_class_type, name: "RWStructuredBuffer<float>", file: !1, line: 11, size: 32, align: 32, elements: !2, templateParams: !27)
!27 = !{!28}
!28 = !DITemplateTypeParameter(name: "element", type: !21)
!29 = !{i32 2, !"Dwarf Version", i32 4}
!30 = !{i32 2, !"Debug Info Version", i32 3}
!31 = !{!"dxc(private) 1.8.0.4775 (d39324e06)"}
!32 = !{!"D:\5Csrc\5Cgpu_inspector\5Ctest\5Cd3d12_triangle\5Cwave.hlsl", !"// A compute shader that refreshes a wave buffer every frame (dxinsp_triangle --compute), so a\0A// capture has a dispatch, a UAV and compute root constants in it. Nothing reads the result.\0A\0A// Root parameter 1, root constants.\0Acbuffer Params : register(b0) {\0A    float time;\0A    uint count;\0A};\0A\0A// Root parameter 0, a descriptor table with one UAV.\0ARWStructuredBuffer<float> wave : register(u0);\0A\0A[numthreads(64, 1, 1)]\0Avoid CSMain(uint3 id : SV_DispatchThreadID) {\0A    if (id.x < count) wave[id.x] = sin(time + id.x * 0.05);\0A}\0A"}
!33 = !{!"D:\5Csrc\5Cgpu_inspector\5Ctest\5Cd3d12_triangle\5Cwave.hlsl"}
!34 = !{!"-E", !"CSMain", !"-T", !"cs_6_0", !"-Zi", !"-Qembed_debug", !"-Fo", !"D:/src/gpu_inspector/build/test/d3d12_triangle/wave_cs.cso"}
!35 = !{i32 1, i32 0}
!36 = !{i32 1, i32 8}
!37 = !{!"cs", i32 6, i32 0}
!38 = !{null, !39, !42, null}
!39 = !{!40}
!40 = !{i32 0, %"class.RWStructuredBuffer<float>"* undef, !"wave", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !41}
!41 = !{i32 1, i32 4}
!42 = !{!43}
!43 = !{i32 0, %Params* undef, !"Params", i32 0, i32 0, i32 1, i32 8, null}
!44 = !{i32 0, %"class.RWStructuredBuffer<float>" undef, !45, %Params undef, !47}
!45 = !{i32 4, !46}
!46 = !{i32 6, !"h", i32 3, i32 0, i32 7, i32 9}
!47 = !{i32 8, !48, !49}
!48 = !{i32 6, !"time", i32 3, i32 0, i32 7, i32 9}
!49 = !{i32 6, !"count", i32 3, i32 4, i32 7, i32 5}
!50 = !{i32 1, void ()* @CSMain, !51}
!51 = !{!52}
!52 = !{i32 0, !2, !2}
!53 = !{void ()* @CSMain, !"CSMain", null, !38, !54}
!54 = !{i32 0, i64 16, i32 4, !55}
!55 = !{i32 64, i32 1, i32 1}
!56 = !DILocalVariable(tag: DW_TAG_arg_variable, name: "id", arg: 1, scope: !4, file: !1, line: 14, type: !7)
!57 = !DIExpression(DW_OP_bit_piece, 0, 32)
!58 = !DILocation(line: 14, column: 19, scope: !4)
!59 = !DILocation(line: 15, column: 16, scope: !60)
!60 = distinct !DILexicalBlock(scope: !4, file: !1, line: 15, column: 9)
!61 = !DILocation(line: 15, column: 14, scope: !60)
!62 = !DILocation(line: 15, column: 9, scope: !4)
!63 = !DILocation(line: 15, column: 40, scope: !60)
!64 = !DILocation(line: 15, column: 47, scope: !60)
!65 = !DILocation(line: 15, column: 52, scope: !60)
!66 = !DILocation(line: 15, column: 45, scope: !60)
!67 = !DILocation(line: 15, column: 36, scope: !60)
!68 = !DILocation(line: 15, column: 34, scope: !60)
!69 = !DILocation(line: 15, column: 23, scope: !60)
!70 = !DILocation(line: 16, column: 1, scope: !4)
