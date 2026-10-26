declare void @print_i32(i32 %v)

define i32 @main() {
entry:
  %q = sdiv i32 -7, 2
  %r = srem i32 -7, 2
  %x = mul i32 %q, 10
  %y = add i32 %x, %r
  call void @print_i32(i32 %y)
  ret i32 0
}
