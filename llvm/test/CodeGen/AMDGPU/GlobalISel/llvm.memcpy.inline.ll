; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -global-isel -mtriple=amdgcn -mem-intrinsic-expand-size=3 %s -o - | FileCheck -check-prefix=GCN %s
; RUN: llc -global-isel -mtriple=amdgcn -mem-intrinsic-expand-size=5 %s -o - | FileCheck -check-prefix=GCN %s

declare void @llvm.memcpy.inline.p1.p1.i32(ptr addrspace(1), ptr addrspace(1), i32, i1 immarg)

define amdgpu_cs void @test(ptr addrspace(1) %dst, ptr addrspace(1) %src) {
; GCN-LABEL: test:
; GCN:       ; %bb.0:
; GCN-NEXT:    s_mov_b32 s2, 0
; GCN-NEXT:    s_mov_b32 s3, 0xf000
; GCN-NEXT:    s_mov_b64 s[0:1], 0
; GCN-NEXT:    buffer_load_ubyte v4, v[2:3], s[0:3], 0 addr64
; GCN-NEXT:    s_waitcnt vmcnt(0)
; GCN-NEXT:    buffer_store_byte v4, v[0:1], s[0:3], 0 addr64
; GCN-NEXT:    s_waitcnt expcnt(0)
; GCN-NEXT:    buffer_load_ubyte v4, v[2:3], s[0:3], 0 addr64 offset:1
; GCN-NEXT:    s_waitcnt vmcnt(0)
; GCN-NEXT:    buffer_store_byte v4, v[0:1], s[0:3], 0 addr64 offset:1
; GCN-NEXT:    s_waitcnt expcnt(0)
; GCN-NEXT:    buffer_load_ubyte v4, v[2:3], s[0:3], 0 addr64 offset:2
; GCN-NEXT:    s_waitcnt vmcnt(0)
; GCN-NEXT:    buffer_store_byte v4, v[0:1], s[0:3], 0 addr64 offset:2
; GCN-NEXT:    buffer_load_ubyte v2, v[2:3], s[0:3], 0 addr64 offset:3
; GCN-NEXT:    s_waitcnt vmcnt(0)
; GCN-NEXT:    buffer_store_byte v2, v[0:1], s[0:3], 0 addr64 offset:3
; GCN-NEXT:    s_endpgm
  call void @llvm.memcpy.inline.p1.p1.i32(ptr addrspace(1) %dst, ptr addrspace(1) %src, i32 4, i1 false)
  ret void
}
