; RUN: llc < %s -mtriple=mips64el -mattr=+soft-float | FileCheck %s

define signext i32 @testmsws(float %x) {
; CHECK-LABEL: testmsws:
; CHECK:       jal     lroundf
entry:
  %0 = tail call i64 @llvm.lround.i64.f32(float %x)
  %conv = trunc i64 %0 to i32
  ret i32 %conv
}

define i64 @testmsxs(float %x) {
; CHECK-LABEL: testmsxs:
; CHECK:       jal     lroundf
entry:
  %0 = tail call i64 @llvm.lround.i64.f32(float %x)
  ret i64 %0
}

define signext i32 @testmswd(double %x) {
; CHECK-LABEL: testmswd:
; CHECK:       jal     lround
entry:
  %0 = tail call i64 @llvm.lround.i64.f64(double %x)
  %conv = trunc i64 %0 to i32
  ret i32 %conv
}

define i64 @testmsxd(double %x) {
; CHECK-LABEL: testmsxd:
; CHECK:       jal     lround
entry:
  %0 = tail call i64 @llvm.lround.i64.f64(double %x)
  ret i64 %0
}

define signext i32 @testmswl(fp128 %x) {
; CHECK-LABEL: testmswl:
; CHECK:       jal     lroundl
entry:
  %0 = tail call i64 @llvm.lround.i64.f128(fp128 %x)
  %conv = trunc i64 %0 to i32
  ret i32 %conv
}

define signext i64 @testmsll(fp128 %x) {
; CHECK-LABEL: testmsll:
; CHECK:       jal     lroundl
entry:
  %0 = tail call i64 @llvm.lround.i64.f128(fp128 %x)
  ret i64 %0
}

declare i64 @llvm.lround.i64.f32(float) nounwind readnone
declare i64 @llvm.lround.i64.f64(double) nounwind readnone
declare i64 @llvm.lround.i64.f128(fp128) nounwind readnone
