define i32 @mix(i8 %a, i16 %b, i32 %c, i64 %d) {
entry:
  %x = zext i8 %a to i32
  %y = zext i16 %b to i32
  %z = add i32 %x, %y
  %w = trunc i64 %d to i32
  %r = add i32 %z, %w
  %r2 = add i32 %r, %c
  ret i32 %r2
}

define i32 @main() {
entry:
  %r = call i32 @mix(i8 1, i16 2, i32 3, i64 4)
  ret i32 %r
}
