define i32 @main() {
entry:
  %p = alloca i32
  store i32 40, ptr %p
  %v = load i32, ptr %p
  %r = add i32 %v, 2
  ret i32 %r
}
