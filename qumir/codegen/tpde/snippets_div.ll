; Qumir integer division: x / 0 == 0, x % 0 == 0, MIN / -1 == MIN, MIN % -1 == 0
; (CreateDefinedDivRem in the LLVM backend). Written in LLVM IR because the C
; equivalent is UB, and clang turns it into branches that EncodeGen rejects.

define i32 @sdivi32(i32 %a, i32 %b) {
  %z = icmp eq i32 %b, 0
  %m = icmp eq i32 %a, -2147483648
  %n = icmp eq i32 %b, -1
  %mn = and i1 %m, %n
  %t = or i1 %z, %mn
  %d = select i1 %t, i32 1, i32 %b
  %q = sdiv i32 %a, %d
  %r = select i1 %z, i32 0, i32 %q
  ret i32 %r
}

define i32 @sremi32(i32 %a, i32 %b) {
  %z = icmp eq i32 %b, 0
  %m = icmp eq i32 %a, -2147483648
  %n = icmp eq i32 %b, -1
  %mn = and i1 %m, %n
  %t = or i1 %z, %mn
  %d = select i1 %t, i32 1, i32 %b
  %r = srem i32 %a, %d
  ret i32 %r
}

define i32 @udivi32(i32 %a, i32 %b) {
  %z = icmp eq i32 %b, 0
  %d = select i1 %z, i32 1, i32 %b
  %q = udiv i32 %a, %d
  %r = select i1 %z, i32 0, i32 %q
  ret i32 %r
}

define i32 @uremi32(i32 %a, i32 %b) {
  %z = icmp eq i32 %b, 0
  %d = select i1 %z, i32 1, i32 %b
  %r = urem i32 %a, %d
  ret i32 %r
}

define i64 @sdivi64(i64 %a, i64 %b) {
  %z = icmp eq i64 %b, 0
  %m = icmp eq i64 %a, -9223372036854775808
  %n = icmp eq i64 %b, -1
  %mn = and i1 %m, %n
  %t = or i1 %z, %mn
  %d = select i1 %t, i64 1, i64 %b
  %q = sdiv i64 %a, %d
  %r = select i1 %z, i64 0, i64 %q
  ret i64 %r
}

define i64 @sremi64(i64 %a, i64 %b) {
  %z = icmp eq i64 %b, 0
  %m = icmp eq i64 %a, -9223372036854775808
  %n = icmp eq i64 %b, -1
  %mn = and i1 %m, %n
  %t = or i1 %z, %mn
  %d = select i1 %t, i64 1, i64 %b
  %r = srem i64 %a, %d
  ret i64 %r
}

define i64 @udivi64(i64 %a, i64 %b) {
  %z = icmp eq i64 %b, 0
  %d = select i1 %z, i64 1, i64 %b
  %q = udiv i64 %a, %d
  %r = select i1 %z, i64 0, i64 %q
  ret i64 %r
}

define i64 @uremi64(i64 %a, i64 %b) {
  %z = icmp eq i64 %b, 0
  %d = select i1 %z, i64 1, i64 %b
  %r = urem i64 %a, %d
  ret i64 %r
}
