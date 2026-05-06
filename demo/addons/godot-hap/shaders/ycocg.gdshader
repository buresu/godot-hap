shader_type canvas_item;

void fragment() {
    vec4 c = texture(TEXTURE, UV);
    float co = c.r - 0.5;
    float cg = c.g - 0.5;
    float y  = c.a;
    COLOR = vec4(y + co - cg, y + cg, y - co - cg, 1.0);
}
