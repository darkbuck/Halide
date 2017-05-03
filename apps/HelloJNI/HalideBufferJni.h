#include <jni.h>

#ifndef HALIDE_BUFFER_JNI_INCLUDED_H
#define HALIDE_BUFFER_JNI_INCLUDED_H
#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jlong JNICALL Java_org_halide_runtime_Buffer_nativeNewBuffer(JNIEnv *,
        jclass, jbyte typeCode, jbyte bits, jshort lanes, jintArray jsizes);

JNIEXPORT jboolean JNICALL Java_org_halide_runtime_Buffer_nativeDeleteBuffer(
        JNIEnv *, jclass, jlong handle);

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeDimensions(JNIEnv *,
        jclass, jlong handle);

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeMin(JNIEnv *,
        jclass, jlong handle, jint i);

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeExtent(JNIEnv *,
        jclass, jlong handle, jint i);

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeStride(JNIEnv *,
        jclass, jlong handle, jint i);

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeWidth(JNIEnv *,
        jclass, jlong handle);

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeHeight(JNIEnv *,
        jclass, jlong handle);

JNIEXPORT jint JNICALL Java_org_halide_runtime_Buffer_nativeChannels(JNIEnv *,
        jclass, jlong handle);

JNIEXPORT jobject JNICALL Java_org_halide_runtime_Buffer_nativeData(JNIEnv *,
        jclass, jlong handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // HALIDE_BUFFER_JNI_INCLUDED_H
