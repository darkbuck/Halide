#include <jni.h>

#include <cstdint>
#include <cstdio>

#include <HalideBuffer.h>

using namespace Halide::Runtime;

namespace {
    // The max number of dimensions stored in the header of the buffer object.
    // It's fine to exceed this - it just incurs a small performance penalty.
    constexpr int kMaxDimensions = 4;
}

using DynamicBuffer = Buffer<void, kMaxDimensions>;

namespace {
    inline DynamicBuffer *asDynamicBufferPtr(jlong handle) {
        return reinterpret_cast<DynamicBuffer*>(handle);
    }

    inline jlong asHandle(DynamicBuffer *buffer_ptr) {
        return reinterpret_cast<jlong>(buffer_ptr);
    }
}

extern "C" {

JNIEXPORT jlong JNICALL Java_org_halide_runtime_Buffer_nativeNewBuffer(
        JNIEnv *env, jclass cls, jbyte typeCode, jbyte bits, jshort lanes,
        jintArray jsizes) {
    halide_type_t type = {
        static_cast<halide_type_code_t>(typeCode),
        static_cast<uint8_t>(bits),
        static_cast<uint16_t>(lanes)
    };

    jsize ndims = env->GetArrayLength(jsizes);
    std::vector<int> sizes(ndims);
    jint *dst = static_cast<jint*>(sizes.data());
    env->GetIntArrayRegion(jsizes, 0, ndims, dst);

    auto buffer_ptr = new DynamicBuffer(type, sizes);
    return asHandle(buffer_ptr);
}

JNIEXPORT jlong JNICALL Java_org_halide_runtime_Buffer_nativeNewBuffer2(
        JNIEnv *env, jclass cls, jbyte typeCode, jbyte bits, jshort lanes,
        jlong nativeDataPointer, jintArray jsizes) {
    halide_type_t type = {
        static_cast<halide_type_code_t>(typeCode),
        static_cast<uint8_t>(bits),
        static_cast<uint16_t>(lanes)
    };

    void *data_pointer = reinterpret_cast<void *>(nativeDataPointer);

    jsize ndims = env->GetArrayLength(jsizes);
    std::vector<int> sizes(ndims);
    jint *dst = static_cast<jint*>(sizes.data());
    env->GetIntArrayRegion(jsizes, 0, ndims, dst);

    auto buffer_ptr = new DynamicBuffer(type, data_pointer, sizes);
    return asHandle(buffer_ptr);
}

JNIEXPORT jboolean JNICALL Java_org_halide_runtime_Buffer_nativeDeleteBuffer(
        JNIEnv *, jclass, jlong handle) {
    // TODO(jiawen): Throw an exception instead?
    if (handle == 0) {
        return false;
    }
    auto buffer_ptr = asDynamicBufferPtr(handle);
    delete buffer_ptr;
    return true;
}

JNIEXPORT jboolean JNICALL Java_org_halide_runtime_Buffer_nativeManagesMemory(
        JNIEnv *, jclass, jlong handle) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->manages_memory();
}

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeDimensions(JNIEnv *,
        jclass, jlong handle) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->dimensions();
}

JNIEXPORT jshort JNICALL Java_org_halide_runtime_Buffer_nativeGetType(
        JNIEnv *env, jclass, jlong handle, jbyteArray codeAndBits) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    halide_type_t type = buffer_ptr->type();

    jbyte *code_and_bits_ptr = env->GetByteArrayElements(codeAndBits,
            /*isCopy=*/nullptr);
    uint8_t *dst = reinterpret_cast<uint8_t *>(code_and_bits_ptr);

    dst[0] = type.code;
    dst[1] = type.bits;

    env->ReleaseByteArrayElements(codeAndBits, code_and_bits_ptr, JNI_COMMIT);

    return static_cast<jshort>(type.lanes);
}

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeMin(JNIEnv *,
        jclass, jlong handle, jint i) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->min(i);
}

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeExtent(JNIEnv *,
        jclass, jlong handle, jint i) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->extent(i);
}

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeStride(JNIEnv *,
        jclass, jlong handle, jint i) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->stride(i);
}

JNIEXPORT jlong JNICALL Java_org_halide_runtime_Buffer_nativeRawBuffer(JNIEnv *,
        jclass, jlong handle) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    halide_buffer_t *raw_buffer = buffer_ptr->raw_buffer();
    return reinterpret_cast<jlong>(raw_buffer);
}

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeWidth(JNIEnv *,
        jclass, jlong handle) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->width();
}

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeHeight(JNIEnv *,
        jclass, jlong handle) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->height();
}

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeChannels(JNIEnv *,
        jclass, jlong handle) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    return buffer_ptr->channels();
}

JNIEXPORT jobject JNICALL Java_org_halide_runtime_Buffer_nativeData(JNIEnv *env,
        jclass, jlong handle) {
    auto buffer_ptr = asDynamicBufferPtr(handle);
    auto data_ptr = reinterpret_cast<uint8_t*>(buffer_ptr->data());
    size_t capacity = buffer_ptr->size_in_bytes();
    return env->NewDirectByteBuffer(data_ptr, capacity);
}

}  // extern "C"
