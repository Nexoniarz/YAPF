// YAPF from Zig: the C header is imported directly.
//   zig run example.zig ../../yapf.c -I../.. -lc -O ReleaseFast
// (Uses only the C API plus std.debug.print, so it works across Zig versions.)
const std = @import("std");
const c = @cImport({
    @cInclude("yapf.h");
    @cInclude("time.h");
});

pub fn main() !void {
    const path: [:0]const u8 = "../../other/YAPF.YAPF";

    var img: *c.yapf_image_t = c.yapf_load(path.ptr) orelse return error.CannotLoad;
    var best: c.clock_t = std.math.maxInt(c.clock_t); // best of 10 decodes
    for (0..10) |_| {
        const t = c.clock();
        const again = c.yapf_load(path.ptr) orelse return error.CannotLoad;
        best = @min(best, c.clock() - t);
        c.yapf_free(img);
        img = again;
    }
    defer c.yapf_free(img);
    std.debug.print("{s}: {d}x{d}, {d} channels, decoded in {d:.2} ms\n", .{
        path, img.width, img.height, img.channels, @as(f64, @floatFromInt(best)) * 1000.0 / @as(f64, @floatFromInt(c.CLOCKS_PER_SEC)),
    });

    var data: ?*anyopaque = null;
    var size: usize = 0;
    if (c.yapf_encode(img, &data, &size) != c.YAPF_OK) return error.EncodeFailed;
    defer c.yapf_free_buffer(data);
    const raw = @as(f64, @floatFromInt(img.width * img.height * img.channels));
    std.debug.print("encoded: {d} bytes, {d:.1}% of the raw pixels\n", .{ size, 100.0 * @as(f64, @floatFromInt(size)) / raw });
}
