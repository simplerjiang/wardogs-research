/* wv2_cb.h — минимальные COM-колбэки без WRL/wil.
 * Один шаблонный класс-компоновщик: хранит лямбду и IUnknown-счётчик.
 * Использование: Callback<IInterface>(lambda).Get()  — как WRL-шный,
 * но без зависимости от <wrl/callback.h>. */
#ifndef WV2_CB_H
#define WV2_CB_H
#include <windows.h>
#include <functional>

template <typename TInterface, typename TFn>
class Wv2Callback : public TInterface {
public:
    Wv2Callback(TFn fn) : fn_(fn), refs_(1) {}

    /* IUnknown */
    ULONG STDMETHODCALLTYPE AddRef() override   { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override  {
        ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(TInterface)) {
            *ppv = static_cast<TInterface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    /* Invoke передаётся в TFn сигнатурой (args...) -> HRESULT */
    template <typename... Args>
    HRESULT invoke(Args... args) { return fn_(args...); }

private:
    TFn   fn_;
    ULONG refs_;
};

/* Фабрика: специализируется под конкретные интерфейсы ниже */
#endif
