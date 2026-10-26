define i32 @main() {
entry:
  %x = add i64 9223372036854775807, 1
  %y = ashr i64 %x, 63
  %z = trunc i64 %y to i32
  ret i32 %z
}
