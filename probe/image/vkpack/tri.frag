#version 450
// gputest: a known colour, so the CPU can check every pixel of the copy.
layout(location = 0) out vec4 o;
void main() { o = vec4(0.25, 0.5, 0.75, 1.0); }
