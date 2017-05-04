package org.halide.runtime;

import java.io.Closeable;
import java.nio.ByteBuffer;
import java.util.Objects;

public class Buffer implements Closeable {

    static {
        System.loadLibrary("HalideRuntimeJni");
    }

    /**
     * A Java version of enum halide_type_code_t.
     */
    public enum TypeCode {
        INT ((byte)0),
        UINT ((byte)1),
        FLOAT ((byte)2),
        HANDLE ((byte)3);

        public final byte halideTypeCode;

        private TypeCode(byte halideTypeCode) {
            this.halideTypeCode = halideTypeCode;
        }
    }

    /**
     * A Java version of struct halide_type_t.
     */
    public static class Type {

        public static final Type INT8 = new Type(TypeCode.INT, (byte)8);
        public static final Type INT16 = new Type(TypeCode.INT, (byte)16);
        public static final Type INT32 = new Type(TypeCode.INT, (byte)32);
        public static final Type INT64 = new Type(TypeCode.INT, (byte)64);
        public static final Type UINT8 = new Type(TypeCode.UINT, (byte)8);
        public static final Type UINT16 = new Type(TypeCode.UINT, (byte)16);
        public static final Type UINT32 = new Type(TypeCode.UINT, (byte)32);
        public static final Type UINT64 = new Type(TypeCode.UINT, (byte)64);
        public static final Type FLOAT16 = new Type(TypeCode.FLOAT, (byte)16);
        public static final Type FLOAT32 = new Type(TypeCode.FLOAT, (byte)32);
        public static final Type FLOAT64 = new Type(TypeCode.FLOAT, (byte)64);
        public final TypeCode typeCode;

        /* bits is interpreted as unsigned by the native code */
        public final byte bits;
        /* lanes is interpreted as unsigned by the native code */
        public final short lanes;

        public Type(TypeCode typeCode, byte bits) {
            this(typeCode, bits, /*lanes=*/(short)1);
        }

        public Type(TypeCode typeCode, byte bits, short lanes) {
            this.typeCode = typeCode;
            this.bits = bits;
            this.lanes = lanes;
        }

        public boolean equals(Object o) {
            if (o == null || !(o instanceof Type)) {
                return false;
            }
            Type ot = (Type)o;
            return (typeCode == ot.typeCode &&
                    bits == ot.bits &&
                    lanes == ot.lanes);
        }

        public int hashCode() {
            return Objects.hash(typeCode, bits, lanes);
        }

        public String toString() {
            StringBuilder sb = new StringBuilder();
            sb.append("{");
            sb.append(" typeCode: ");
            sb.append(typeCode.toString());
            sb.append(", bits: " + bits);
            sb.append(", lanes: " + lanes);
            sb.append(" }");
            return sb.toString();
        }
    }

    private long nativeHandle;

    /**
     * Construct a new Buffer and allocate native memory. The underlying native Buffer owns the
     * memory ({@link managesMemory()} returns true).
     *
     * @param type Type for each element.
     * @param sizes The extent of each dimension.
     */
    public Buffer(Type type, int... sizes) {
        nativeHandle = nativeNewBuffer(type.typeCode.halideTypeCode, type.bits, type.lanes, sizes);
    }

    /**
     * Construct a new Buffer wrapping native memory. Assumes dense row-major packing and a min
     * coordinate of zero. Does not take ownership of the data and does not set the host_dirty flag.
     *
     * @param type Type for each element.
     * @param nativeDataPointer An address pointing to the native data array, cast to a long.
     * @param sizes The extent of each dimension.
     */
    public Buffer(Type type, long nativeDataPointer, int... sizes) {
        nativeHandle = nativeNewBuffer2(type.typeCode.halideTypeCode, type.bits, type.lanes,
                nativeDataPointer, sizes);
    }

    // TODO(jiawen): Add constructor for taking a direct ByteBuffer with strides to create a
    // non-owning Buffer view of native memory. This involves wrapping
    // halide_dimension_t.

    // TODO(jiawen): copy() might be tricky to implement.

    public boolean managesMemory() {
        return nativeManagesMemory(nativeHandle);
    }

    public int dimensions() {
        return nativeDimensions(nativeHandle);
    }

    public Type type() {
        // bytes[0] will store the typecode.
        // bytes[1] will store the number of bits.
        byte[] codeAndBits = new byte[2];
        short lanes = nativeGetType(nativeHandle, codeAndBits);

        TypeCode typeCode = TypeCode.values()[codeAndBits[0]];
        return new Type(typeCode, codeAndBits[1], lanes);
    }

    public int min(int i) {
        if (i < 0 || i >= dimensions()) {
            throw new ArrayIndexOutOfBoundsException(
                    String.format("Attempted to access min %d on a buffer with %d dimensions.",
                        i, dimensions()));
        }
        return nativeMin(nativeHandle, i);
    }

    public int extent(int i) {
        if (i < 0 || i >= dimensions()) {
            throw new ArrayIndexOutOfBoundsException(
                    String.format("Attempted to access extent %d on a buffer with %d dimensions.",
                        i, dimensions()));
        }
        return nativeExtent(nativeHandle, i);
    }

    public int stride(int i) {
        if (i < 0 || i >= dimensions()) {
            throw new ArrayIndexOutOfBoundsException(
                    String.format("Attempted to access stride %d on a buffer with %d dimensions.",
                        i, dimensions()));
        }
        return nativeStride(nativeHandle, i);
    }

    /**
     * Returns a handle to the native halide_buffer_t*. This is the handle that will be eventually
     * passed to pipelines for input and output buffers.
     */
    public long rawBuffer() {
        return nativeRawBuffer(nativeHandle);
    }

    public int width() {
        return nativeWidth(nativeHandle);
    }

    public int height() {
        return nativeHeight(nativeHandle);
    }

    public int channels() {
        return nativeChannels(nativeHandle);
    }

    /**
     * Return a non-owning ByteBuffer <b>view</b> of the underlying data.
     */
    public ByteBuffer data() {
        return nativeData(nativeHandle);
    }

    /**
     * Return a non-owning <b>read-only</b> ByteBuffer <b>view</b> of the underlying data.
     */
    public ByteBuffer readOnlyData() {
        return nativeData(nativeHandle).asReadOnlyBuffer();
    }

    /**
     * Release the native handle associated with this Buffer.
     */
    @Override
    public void close() {
        if (nativeHandle != 0) {
            nativeDeleteBuffer(nativeHandle);
            nativeHandle = 0;
        }
    }

    @Override
    protected void finalize() {
        close();
    }

    private static native long nativeNewBuffer(byte typeCode, byte bits, short lanes, int... sizes);

    private static native long nativeNewBuffer2(byte typeCode, byte bits, short lanes,
            long nativeDataPointer, int... sizes);

    private static native boolean nativeDeleteBuffer(long handle);

    private static native boolean nativeManagesMemory(long handle);

    private static native int nativeDimensions(long handle);

    // Stores typeCode in codeAndBits[0]. Stores number of bits in codeAndBits[1].
    // Returns the number of lanes.
    private static native short nativeGetType(long handle, byte[] codeAndBits);

    private static native int nativeMin(long handle, int i);

    private static native int nativeExtent(long handle, int i);

    private static native int nativeStride(long handle, int i);

    private static native long nativeRawBuffer(long nativeHandle);

    private static native int nativeWidth(long handle);

    private static native int nativeHeight(long handle);

    private static native int nativeChannels(long handle);

    private static native ByteBuffer nativeData(long handle);

    public static void main(String[] args) {
        Buffer b0 = new Buffer(Buffer.Type.UINT8);
        System.out.println("b0 dimensions = " + b0.dimensions());

        Buffer b1= new Buffer(Buffer.Type.UINT8, 640, 480);
        System.out.println("b1.dimensions = " + b1.dimensions());
        for (int i = 0; i < b1.dimensions(); ++i) {
            System.out.println("b1.extent[" + i + "] = " + b1.extent(i));
        }
    }
}
