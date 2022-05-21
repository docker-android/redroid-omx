/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "RedroidOMXPlugin"
#include <utils/Log.h>

#include <android-base/properties.h>

#include <media/stagefright/omx/RedroidOMXPlugin.h>
#include <media/stagefright/omx/RedroidOMXComponent.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>

#include <dlfcn.h>

#define d(...) ALOGD(__VA_ARGS__)

namespace android {

static const struct {
    const char *mName;
    const char *mLibNameSuffix;
    const char *mRole;

} kComponents[] = {
    { "OMX.redroid.h264.encoder", "avcenc", "video_encoder.avc" },
};

static const size_t kNumComponents =
    sizeof(kComponents) / sizeof(kComponents[0]);

extern "C" OMXPluginBase* createOMXPlugin() {
    d(__func__);
    return new RedroidOMXPlugin();
}

extern "C" void destroyOMXPlugin(OMXPluginBase* plugin) {
    d(__func__);
    delete plugin;
}

RedroidOMXPlugin::RedroidOMXPlugin() { }

OMX_ERRORTYPE RedroidOMXPlugin::makeComponentInstance(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component) {
    d("%s, name: %s", __func__, name);

    for (size_t i = 0; i < kNumComponents; ++i) {
        if (strcmp(name, kComponents[i].mName)) {
            continue;
        }

        AString libName = "libstagefright_redroid_";
        libName.append(kComponents[i].mLibNameSuffix);
        libName.append(".so");

        // RTLD_NODELETE means we keep the shared library around forever.
        // this eliminates thrashing during sequences like loading soundpools.
        // It also leaves the rest of the logic around the dlopen()/dlclose()
        // calls in this file unchanged.
        //
        // Implications of the change:
        // -- the codec process (where this happens) will have a slightly larger
        //    long-term memory footprint as it accumulates the loaded shared libraries.
        //    This is expected to be a small amount of memory.
        // -- plugin codecs can no longer (and never should have) depend on a
        //    free reset of any static data as the library would have crossed
        //    a dlclose/dlopen cycle.
        //

        void *libHandle = dlopen(libName.c_str(), RTLD_NOW|RTLD_NODELETE);

        if (libHandle == NULL) {
            ALOGE("unable to dlopen %s: %s", libName.c_str(), dlerror());

            return OMX_ErrorComponentNotFound;
        }

        typedef RedroidOMXComponent *(*CreateRedroidOMXComponentFunc)(
                const char *, const OMX_CALLBACKTYPE *,
                OMX_PTR, OMX_COMPONENTTYPE **);

        CreateRedroidOMXComponentFunc createRedroidOMXComponent =
            (CreateRedroidOMXComponentFunc)dlsym(
                    libHandle,
                    "createRedroidOMXComponent");

        if (createRedroidOMXComponent == NULL) {
            dlclose(libHandle);
            libHandle = NULL;

            return OMX_ErrorComponentNotFound;
        }

        sp<RedroidOMXComponent> codec =
            (*createRedroidOMXComponent)(name, callbacks, appData, component);

        if (codec == NULL) {
            dlclose(libHandle);
            libHandle = NULL;

            return OMX_ErrorInsufficientResources;
        }

        OMX_ERRORTYPE err = codec->initCheck();
        if (err != OMX_ErrorNone) {
            dlclose(libHandle);
            libHandle = NULL;

            return err;
        }

        codec->incStrong(this);
        codec->setLibHandle(libHandle);

        return OMX_ErrorNone;
    }

    return OMX_ErrorInvalidComponentName;
}

OMX_ERRORTYPE RedroidOMXPlugin::destroyComponentInstance(
        OMX_COMPONENTTYPE *component) {
    d(__func__);
    RedroidOMXComponent *me =
        (RedroidOMXComponent *)
            ((OMX_COMPONENTTYPE *)component)->pComponentPrivate;

    me->prepareForDestruction();

    void *libHandle = me->libHandle();

    CHECK_EQ(me->getStrongCount(), 1);
    me->decStrong(this);
    me = NULL;

    dlclose(libHandle);
    libHandle = NULL;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE RedroidOMXPlugin::enumerateComponents(
        OMX_STRING name,
        size_t /* size */,
        OMX_U32 index) {
    if (!android::base::GetBoolProperty("sys.use_redroid_omx", false)) return OMX_ErrorNoMore;
    if (index >= kNumComponents) {
        return OMX_ErrorNoMore;
    }

    strcpy(name, kComponents[index].mName);

    return OMX_ErrorNone;
}

OMX_ERRORTYPE RedroidOMXPlugin::getRolesOfComponent(
        const char *name,
        Vector<String8> *roles) {
    for (size_t i = 0; i < kNumComponents; ++i) {
        if (strcmp(name, kComponents[i].mName)) {
            continue;
        }

        roles->clear();
        roles->push(String8(kComponents[i].mRole));

        return OMX_ErrorNone;
    }

    return OMX_ErrorInvalidComponentName;
}

}  // namespace android
