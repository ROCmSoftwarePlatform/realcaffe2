#define NO_IMPORT_ARRAY

#include "pybind_state.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "caffe2/core/common_miopen.h"
#include "caffe2/core/context_hip.h"
#include "caffe2/operators/operator_fallback_hip.h"

namespace caffe2 {
namespace python {

REGISTER_HIP_OPERATOR(Python, GPUFallbackOp<PythonOp<CPUContext, false>>);
REGISTER_HIP_OPERATOR(PythonGradient, GPUFallbackOp<PythonGradientOp<CPUContext, false>>);

REGISTER_HIP_OPERATOR(PythonDLPack, PythonOp<HIPContext, true>);
REGISTER_HIP_OPERATOR(PythonDLPackGradient, PythonGradientOp<HIPContext, true>);

REGISTER_BLOB_FETCHER((TypeMeta::Id<TensorHIP>()), TensorFetcher<HIPContext>);
REGISTER_BLOB_FEEDER(HIP, TensorFeeder<HIPContext>);

namespace py = pybind11;

void addHIPGlobalMethods(py::module& m)
{
    m.def("num_hip_devices", &NumHipDevices);
    m.def("set_default_gpu_id", &SetDefaultGPUID);
    m.def("get_default_gpu_id", &GetDefaultGPUID);
    m.def("get_hip_version", &HipVersion);
    m.def("get_miopen_version", &miopenCompiledVersion);
    m.def("get_hip_peer_access_pattern", []() {
        std::vector<std::vector<bool>> pattern;
        CAFFE_ENFORCE(caffe2::GetHipPeerAccessPattern(&pattern));
        return pattern;
    });
    m.def("get_device_properties", [](int deviceid) {
        auto& prop = GetDeviceProperty(deviceid);
        std::map<std::string, py::object> obj;
        obj["name"]  = py::cast(prop.name);
        obj["major"] = py::cast(prop.major);
        obj["minor"] = py::cast(prop.minor);
        return obj;
    });
};

void addHIPObjectMethods(py::module& m)
{
    py::class_<DLPackWrapper<HIPContext>>(m, "DLPackTensorHIP")
        .def_property_readonly("data",
                               [](DLPackWrapper<HIPContext>* t) -> py::object {
                                   CAFFE_ENFORCE_EQ(t->device_option.device_type(),
                                                    HIP,
                                                    "Expected HIP device option for HIP tensor");

                                   return t->data();
                               },
                               "Return DLPack tensor with tensor's data.")
        .def("feed",
             [](DLPackWrapper<HIPContext>* t, py::object obj) {
                 CAFFE_ENFORCE_EQ(t->device_option.device_type(),
                                  HIP,
                                  "Expected HIP device option for HIP tensor");
                 t->feed(obj);
             },
             "Copy data from given DLPack tensor into this tensor.")
        .def_property_readonly("_shape",
                               [](const DLPackWrapper<HIPContext>& t) { return t.tensor->dims(); })
        .def("_reshape", [](DLPackWrapper<HIPContext>* t, std::vector<TIndex> dims) {
            t->tensor->Resize(dims);
        });
}

PYBIND11_MODULE(caffe2_pybind11_state_hip, m)
{
    m.doc() = "pybind11 stateful interface to Caffe2 workspaces - GPU edition";

    addGlobalMethods(m);
    addHIPGlobalMethods(m);
    addObjectMethods(m);
    addHIPObjectMethods(m);
}
} // namespace python
} // namespace caffe2
