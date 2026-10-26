define i32 @main() {
entry:
  %a = call i32 @putchar(i32 72)
  %b = call i32 @putchar(i32 105)
  %c = call i32 @putchar(i32 10)
  ret i32 0
}
