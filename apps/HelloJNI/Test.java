import org.halide.runtime.Buffer;

import java.nio.ByteBuffer;

public class Test {
    static {
        System.loadLibrary("MyFilter");
    }

    public static void testTypeEquality() {
        Buffer b0 = new Buffer(Buffer.Type.UINT8, 64, 4, 5);
        Buffer b1 = new Buffer(Buffer.Type.UINT8, 32, 16, 3);

        assert(b0.type().equals(Buffer.Type.UINT8));
        assert(b0.type().equals(b1.type()));
    }

    public static void testDimensions() {
        Buffer b0 = new Buffer(Buffer.Type.UINT8, 64, 4, 5, 1, 7);
        Buffer b1 = new Buffer(Buffer.Type.UINT8, 32, 16, 3, 1, 9);

        assert(b0.dimensions() == 5);
        assert(b1.dimensions() == 5);
        assert(b0.dimensions() == b1.dimensions());
    }

    public static void main(String[] args) {

        testTypeEquality();
        testDimensions();

        Buffer b = new Buffer(Buffer.Type.UINT8, 4, 640, 480);
        Buffer b2 = new Buffer(Buffer.Type.UINT8, 32, 16, 3);

        System.out.println("b.dimensions = " + b.dimensions());
        for (int i = 0; i < b.dimensions(); ++i) {
            System.out.println("b.min[" + i + "] = " + b.min(i));
            System.out.println("b.extent[" + i + "] = " + b.extent(i));
            System.out.println("b.stride[" + i + "] = " + b.stride(i));
        }

        for (float t = 0; t < 1; t += 1.0f/30.f) {
            MyFilter.filter(t, b);
            ByteBuffer buffer = b.data();
            int x = 320;
            int y = 240;
            int width = 640;
            //float middle_pixel = buffer.get(4 * y * width + x);
            byte middle_pixel = buffer.get(50000);
            System.out.println("t = " + t + " middle_pixel = " + middle_pixel);
        }
    }
}
