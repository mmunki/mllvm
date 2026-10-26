define i32 @main() {
entry:
  %a = sext i8 -2 to i32
  %b = xor i32 %a, -1
  %c = trunc i32 %b to i16
  %d = zext i16 %c to i32
  %e = and i32 %d, 255
  ret i32 %e
}
