/*
    init.cpp -- Implementation of <Dr.Jit array>.__init__() and
    other initializion routines like dr.zero(), dr.empty(), etc.

    Dr.Jit: A Just-In-Time-Compiler for Differentiable Rendering
    Copyright 2023, Realistic Graphics Lab, EPFL.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include "meta.h"
#include "base.h"
#include "memop.h"

/// Forward declaration
static bool array_init_seq(PyObject *self, const ArraySupplement &s, PyObject *seq);

/// Constructor for all dr.ArrayBase subclasses (except tensors)
int tp_init_array(PyObject *self, PyObject *args, PyObject *kwds) noexcept {
    PyTypeObject *self_tp = Py_TYPE(self);
    const ArraySupplement &s = supp(self_tp);
    Py_ssize_t argc = NB_TUPLE_GET_SIZE(args);
    ArraySupplement::SetItem set_item = s.set_item;

    try {
        raise_if(kwds, "Constructor does not take keyword arguments.");

        if (argc == 0) {
            // Default initialization, e.g., ``Array3f()``
            nb::detail::nb_inst_zero(self);
            return 0;
        } else if (argc > 1) {
            // Initialize from argument list, e.g., ``Array3f(1, 2, 3)``
            raise_if(!array_init_seq(self, s, args),
                     "Could not initialize array from argument list.");
            return 0;
        } else {
            // Initialize from a single element, e.g., ``Array3f(other_array)``
            // or ``Array3f(1.0)``
            PyObject *arg = NB_TUPLE_GET_ITEM(args, 0);
            PyTypeObject *arg_tp = Py_TYPE(arg);
            bool try_sequence_import = true;

            // Initialization from another Dr.Jit array
            if (is_drjit_type(arg_tp)) {
                // Copy-constructor
                if (arg_tp == self_tp) {
                    nb::detail::nb_inst_copy(self, arg);
                    return 0;
                } else {
                    const ArraySupplement &s_arg = supp(arg_tp);

                    ArrayMeta m_self = s,
                              m_arg  = s_arg;

                    // Potentially do a cast
                    ArrayMeta m_temp = s_arg;
                    m_temp.type = s.type;
                    if (m_temp == m_self && s.cast) {
                        s.cast(inst_ptr(arg), (VarType) s_arg.type, inst_ptr(self));
                        nb::inst_mark_ready(self);
                        return 0;
                    }

                    // Potentially load from the CPU
                    m_temp = s;
                    m_temp.backend = (uint64_t) JitBackend::Invalid;
                    m_temp.is_vector = true;

                    if (m_temp == m_arg && s.init_data && s_arg.data) {
                        dr::ArrayBase *arg_p = inst_ptr(arg);
                        size_t len = s_arg.len(arg_p);
                        void *data = s_arg.data(arg_p);
                        s.init_data(len, data, inst_ptr(self));
                        nb::inst_mark_ready(self);
                        return 0;
                    }

                    // Disallow inefficient element-by-element imports of JIT arrays
                    if (s.ndim == 1 && s_arg.ndim == 1 && s.shape[0] == DRJIT_DYNAMIC && s_arg.shape[0] == DRJIT_DYNAMIC) {
                        try_sequence_import = false;
                    } else {
                        // Always broadcast when the element type is one of the sub-elements
                        PyTypeObject *cur_tp = (PyTypeObject *) s.value;
                        while (cur_tp) {
                            if (arg_tp == cur_tp) {
                                try_sequence_import = false;
                                break;
                            }
                            cur_tp = (PyTypeObject *) supp(cur_tp).value;
                        }
                    }
                }
            }

            // Try to construct from a sequence/iterable type
            if (try_sequence_import && array_init_seq(self, s, arg))
                return 0;

            // No sequence/iterable type, try broadcasting
            Py_ssize_t size = s.shape[0];
            raise_if(size == 0,
                     "Input has the wrong size (expected 0 elements, got 1).");

            nb::object element;
            PyObject *value_type = s.value;

            if (s.is_matrix)
                value_type = supp(value_type).value;

            if (arg_tp == (PyTypeObject *) s.value) {
                element = nb::borrow(arg);
            } else {
                PyObject *args[2] = { nullptr, arg };
                element = nb::steal(
                    NB_VECTORCALL(value_type, args + 1,
                                  1 | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr));
                if (NB_UNLIKELY(!element.is_valid())) {
                    nb::error_scope scope;
                    nb::str arg_tp_name = nb::type_name(arg_tp);
                    nb::detail::raise(
                        "Broadcast from type '%s' failed.%s",
                        arg_tp_name.c_str(),
                        try_sequence_import
                            ? ""
                            : " Refused to perform an inefficient "
                              "element-by-element copy.");
                }
            }

            if (size == DRJIT_DYNAMIC) {
                if (s.init_const) {
                    s.init_const(1, element.ptr(), inst_ptr(self));
                    nb::inst_mark_ready(self);
                    return 0;
                }

                size = 1;
                s.init(1, inst_ptr(self));
                nb::inst_mark_ready(self);
            } else {
                nb::inst_zero(self);
            }

            if (s.is_complex) {
                nb::float_ zero(0.0);
                raise_if(set_item(self, 0, element.ptr()) ||
                         set_item(self, 1, zero.ptr()),
                         "Item assignment failed.");
            } else if (s.is_quaternion) {
                nb::float_ zero(0.0);
                raise_if(set_item(self, 0, zero.ptr()) ||
                         set_item(self, 1, zero.ptr()) ||
                         set_item(self, 2, zero.ptr()) ||
                         set_item(self, 3, element.ptr()),
                         "Item assignment failed.");
            } else if (s.is_matrix) {
                nb::float_ zero(0.0);

                for (Py_ssize_t i = 0; i < size; ++i) {
                    nb::object col = nb::steal(s.item(self, i));
                    for (Py_ssize_t j = 0; j < size; ++j)
                        col[j] = (i == j) ? element : zero;
                }
            } else {
                for (Py_ssize_t i = 0; i < size; ++i)
                    raise_if(set_item(self, i, element.ptr()),
                             "Item assignment failed.");
            }

            return 0;
        }
    } catch (const std::exception &e) {
        nb::str tp_name = nb::type_name(self_tp);
        nb::chain_error(PyExc_TypeError, "%U.__init__(): %s", tp_name.ptr(), e.what());
        return -1;
    }
}

static bool array_init_seq(PyObject *self, const ArraySupplement &s, PyObject *seq) {
    ssizeargfunc sq_item = nullptr;
    lenfunc sq_length = nullptr;

    PyTypeObject *tp = Py_TYPE(seq);
#if defined(Py_LIMITED_API)
    sq_length = (lenfunc) PyType_GetSlot(tp, Py_sq_length);
    sq_item = (ssizeargfunc) PyType_GetSlot(tp, Py_sq_item);
#else
    PySequenceMethods *sm = tp->tp_as_sequence;
    if (sm) {
        sq_length = sm->sq_length;
        sq_item = sm->sq_item;
    }
#endif

    if (!sq_length || !sq_item) {
        // Special case for general iterable types. Handled recursively
        getiterfunc tp_iter;

#if defined(Py_LIMITED_API)
        tp_iter = (getiterfunc) PyType_GetSlot(tp, Py_tp_iter);
#else
        tp_iter = tp->tp_iter;
#endif

        if (tp_iter) {
            nb::object seq2 = nb::steal(PySequence_List(seq));
            raise_if(!seq2.is_valid(),
                     "Could not convert iterable into a sequence.");
            return array_init_seq(self, s, seq2.ptr());
        }

        return false;
    }

    Py_ssize_t size = sq_length(seq);
    raise_if(size < 0, "Unable to determine the size of the given sequence.");

    bool is_dynamic = s.shape[0] == DRJIT_DYNAMIC;
    raise_if(!is_dynamic && s.shape[0] != size,
             "Input has the wrong size (expected %u elements, got %zd).",
             (unsigned) s.shape[0], size);

    if (size == 1 && s.init_const) {
        nb::object o = nb::steal(sq_item(seq, 0));
        raise_if(!o.is_valid(), "Item retrival failed.");
        s.init_const((size_t) size, o.ptr(), inst_ptr(self));
        nb::inst_mark_ready(self);
        return true;
    }

    if (s.ndim == 1 && s.init_data) {
        size_t byte_size = jit_type_size((VarType) s.type) * (size_t) size;
        std::unique_ptr<uint8_t[]> storage(new uint8_t[byte_size]);
        bool fail = false;

        #define FROM_SEQ_IMPL(T)                                           \
        {                                                                  \
            nb::detail::make_caster<T> caster;                             \
            T *p = (T *) storage.get();                                    \
            for (Py_ssize_t i = 0; i < size; ++i) {                        \
                nb::object o = nb::steal(sq_item(seq, i));                 \
                if (NB_UNLIKELY(!o.is_valid() ||                           \
                    !caster.from_python(o,                                 \
                                        (uint8_t) nb::detail::cast_flags:: \
                                            convert, nullptr))) {          \
                    fail = true;                                           \
                    break;                                                 \
                }                                                          \
                p[i] = caster.value;                                       \
            }                                                              \
        }

        switch ((VarType) s.type) {
            case VarType::Bool:    FROM_SEQ_IMPL(bool);     break;
            case VarType::Float32: FROM_SEQ_IMPL(float);    break;
            case VarType::Float64: FROM_SEQ_IMPL(double);   break;
            case VarType::Int32:   FROM_SEQ_IMPL(int32_t);  break;
            case VarType::UInt32:  FROM_SEQ_IMPL(uint32_t); break;
            case VarType::Int64:   FROM_SEQ_IMPL(int64_t);  break;
            case VarType::UInt64:  FROM_SEQ_IMPL(uint64_t); break;
            default: fail = true;
        }

        raise_if(fail, "Could not construct from sequence (invalid type in input).");

        s.init_data((size_t) size, storage.get(), inst_ptr(self));
        nb::inst_mark_ready(self);

        return true;
    }

    if (is_dynamic) {
        s.init((size_t) size, inst_ptr(self));
        nb::inst_mark_ready(self);
    } else {
        nb::inst_zero(self);
    }

    ArraySupplement::SetItem set_item = s.set_item;
    for (Py_ssize_t i = 0; i < size; ++i) {
        nb::object o = nb::steal(sq_item(seq, i));
        raise_if(!o.is_valid(),
                 "Item retrieval failed.");
        raise_if(set_item(self, i, o.ptr()),
                 "Item assignment failed.");
    }

    return true;
}

nb::object full_alt(nb::type_object dtype, nb::handle value, size_t size);
nb::object empty_alt(nb::type_object dtype, size_t size);

int tp_init_tensor(PyObject *self, PyObject *args, PyObject *kwds) noexcept {
    PyTypeObject *self_tp = Py_TYPE(self);

    try {
        PyObject *array = nullptr, *shape = nullptr;
        const char *kwlist[3] = { "array", "shape", nullptr };
        raise_if(!PyArg_ParseTupleAndKeywords(args, kwds, "|OO!",
                                              (char **) kwlist, &array,
                                              &PyTuple_Type, &shape),
                 "Invalid tensor constructor arguments.");

        const ArraySupplement &s = supp(self_tp);

        if (!shape && !array) {
            nb::detail::nb_inst_zero(self);
            s.tensor_shape(inst_ptr(self)).push_back(0);
            return 0;
        }

        raise_if(!array, "Input array must be specified.");

        PyTypeObject *array_tp = Py_TYPE(array);

        // Same type -> copy constructor
        if (array_tp == self_tp) {
            nb::detail::nb_inst_copy(self, array);
            return 0;
        }

        nb::detail::nb_inst_zero(self);
        dr_vector<size_t> &shape_vec = s.tensor_shape(inst_ptr(self));

        nb::object args_2;
        if (!shape) {
            // Infer the shape of an arbitrary data structure & flatten it
            VarType vt = (VarType) s.type;
            nb::object flat = ravel(array, 'C', &shape_vec, nullptr, &vt);
            args_2 = nb::make_tuple(flat);
        } else {
            // Shape is given, require flat input
            args_2 = nb::make_tuple(nb::handle(array));
            shape_vec.resize((size_t) NB_TUPLE_GET_SIZE(shape));

            for (size_t i = 0; i < shape_vec.size(); ++i) {
                PyObject *o = NB_TUPLE_GET_ITEM(shape, (Py_ssize_t) i);
                size_t rv = PyLong_AsSize_t(o);
                raise_if(rv == (size_t) -1, "Invalid shape tuple.");
                shape_vec[i] = rv;
            }
        }

        nb::object self_array = nb::steal(s.tensor_array(self));
        int rv = tp_init_array(self_array.ptr(), args_2.ptr(), nullptr);
        auto [ready, destruct] = nb::inst_state(self_array);
        (void) destruct;
        nb::inst_set_state(self_array, ready, false);
        raise_if(rv, "Tensor storage initialization failed.");

        // Double-check that the size makes sense
        size_t size_exp = 1, size = nb::len(self_array);
        for (size_t i = 0; i < shape_vec.size(); ++i)
            size_exp *= shape_vec[i];

        raise_if(size != size_exp,
                 "Input array has the wrong number of entries (got %zu, "
                 "expected %zu).", size, size_exp);

        return 0;
    } catch (nb::python_error &e) {
        nb::str tp_name = nb::type_name(self_tp);
        e.restore();
        nb::chain_error(PyExc_TypeError, "%U.__init__(): internal error.", tp_name.ptr());
        return -1;
    } catch (const std::exception &e) {
        nb::str tp_name = nb::type_name(self_tp);
        nb::chain_error(PyExc_TypeError, "%U.__init__(): %s", tp_name.ptr(), e.what());
        return -1;
    }
}

// Forward declaration
nb::object full(nb::handle dtype, nb::handle value, size_t ndim,
                const size_t *shape);

nb::object full(nb::handle dtype, nb::handle value,
                std::vector<size_t> &shape) {
    return full(dtype, value, shape.size(), shape.data());
}

nb::object full(nb::handle dtype, nb::handle value, size_t size) {
    std::vector<size_t> shape;

    if (is_drjit_type(dtype)) {
        const ArraySupplement &s = supp(dtype);
        shape.resize(s.ndim);

        for (size_t i = 0; i < s.ndim; ++i) {
            size_t k = s.shape[i];
            if (k == DRJIT_DYNAMIC)
                k = (i == s.ndim - 1) ? size : 1;
            shape[i] = k;
        }
    } else {
        shape.resize(1);
        shape[0] = size;
    }

    return full(dtype, value, shape);
}


nb::object full(nb::handle dtype, nb::handle value, size_t ndim, const size_t *shape) {
    if (is_drjit_type(dtype)) {
        const ArraySupplement &s = supp(dtype);

        bool fail = s.ndim != ndim;
        if (!fail) {
            for (size_t i = 0; i < ndim; ++i)
                fail |= s.shape[i] != DRJIT_DYNAMIC && s.shape[i] != shape[i];
        }

        if (fail)
            nb::detail::raise(
                "The provided 'shape' and 'dtype' parameters are incompatible.");

        nb::object result = nb::inst_alloc(dtype);

        if (s.init_const && value.is_valid()) {
            if ((VarType) s.type == VarType::Bool && value.type().is(&PyLong_Type))
                value = nb::cast<int>(value) ? Py_True : Py_False;

            s.init_const(shape[0], value.ptr(),
                         inst_ptr(result));
            nb::inst_mark_ready(result);
            return result;
        }

        if (s.shape[0] == DRJIT_DYNAMIC) {
            s.init(shape[0], inst_ptr(result));
            nb::inst_mark_ready(result);
        } else {
            nb::inst_zero(result);
        }

        if (!value.is_valid() && ndim == 1) {
            return result;
        } else {
            ArraySupplement::SetItem set_item = s.set_item;
            nb::object o;
            for (size_t i = 0; i < shape[0]; ++i) {
                if (i == 0 || !value.is_valid())
                    o = full(s.value, value, ndim - 1, shape + 1);
                set_item(result.ptr(), i, o.ptr());
            }
        }

        return result;
    } else if (dtype.is(&PyLong_Type) || dtype.is(&PyFloat_Type) || dtype.is(&PyBool_Type)) {
        if (value.is_valid())
            return dtype(value);
        else
            return dtype(0);
    } else {
        nb::object dstruct = nb::getattr(dtype, "DRJIT_STRUCT", nb::handle());
        if (dstruct.is_valid() && dstruct.type().is(&PyDict_Type)) {
            nb::dict dstruct_dict = nb::borrow<nb::dict>(dstruct);
            nb::object result = dtype();

            for (auto [k, v] : dstruct_dict) {
                if (!v.is_type())
                    throw nb::type_error("DRJIT_STRUCT invalid, expected type keys.");

                nb::object entry;
                if (is_drjit_type(v) && ndim == 1)
                    entry = full(v, value, shape[0]);
                else
                    entry = full(v, value, ndim, shape);

                nb::setattr(result, k, entry);
            }

            return result;
        }

        throw nb::type_error("Unsupported dtype.");
    }
}

nb::object arange(const nb::type_object_t<dr::ArrayBase> &dtype,
                  Py_ssize_t start, Py_ssize_t end, Py_ssize_t step) {
    const ArraySupplement &s = supp(dtype);

    if (s.ndim != 1 || s.shape[0] != DRJIT_DYNAMIC)
        throw nb::type_error("drjit.arange(): unsupported dtype -- must "
                             "be a dynamically sized 1D array.");

    VarType vt = (VarType) s.type;
    if (vt == VarType::Bool || vt == VarType::Pointer)
        throw nb::type_error("drjit.arange(): unsupported dtype -- must "
                             "be an arithmetic type.");

    Py_ssize_t size = (end - start + step - (step > 0 ? 1 : -1)) / step;
    ArrayMeta meta = s;
    meta.type = (uint16_t) VarType::UInt32;

    nb::handle counter_tp = meta_get_type(meta);
    const ArraySupplement &counter_s = supp(counter_tp);

    if (!counter_s.init_counter)
        throw nb::type_error("drjit.arange(): unsupported dtype.");

    if (size == 0)
        return dtype();
    else if (size < 0)
        nb::detail::raise("drjit.arange(): size cannot be negative.");

    nb::object result = nb::inst_alloc(counter_tp);
    counter_s.init_counter((size_t) size, inst_ptr(result));
    nb::inst_mark_ready(result);

    if (start == 0 && step == 1)
        return dtype(result);
    else
        return fma(dtype(result), dtype(step), dtype(start));
}

nb::object linspace(const nb::type_object_t<dr::ArrayBase> &dtype,
                    double start, double stop, size_t size, bool endpoint) {
    const ArraySupplement &s = supp(dtype);

    if (s.ndim != 1 || s.shape[0] != DRJIT_DYNAMIC)
        throw nb::type_error("drjit.linspace(): unsupported dtype -- must "
                             "be a dynamically sized 1D array.");

    VarType vt = (VarType) s.type;
    if (vt != VarType::Float16 && vt != VarType::Float32 && vt != VarType::Float64)
        throw nb::type_error("drjit.linspace(): unsupported dtype -- must "
                             "be an floating point type.");

    ArrayMeta meta = s;
    meta.type = (uint16_t) VarType::UInt32;

    nb::handle counter_tp = meta_get_type(meta);
    const ArraySupplement &counter_s = supp(counter_tp);

    if (!counter_s.init_counter)
        throw nb::type_error("drjit.linspace(): unsupported dtype.");

    if (size == 0)
        return dtype();

    nb::object result = nb::inst_alloc(counter_tp);
    counter_s.init_counter((size_t) size, inst_ptr(result));
    nb::inst_mark_ready(result);

    double step = (stop - start) / (size - ((endpoint && size > 0) ? 1 : 0));
    return fma(dtype(result), dtype(step), dtype(start));
}

void export_init(nb::module_ &m) {
    m.def("empty",
          [](nb::type_object dtype, size_t size) {
              return full(dtype, nb::handle(), size);
          }, "dtype"_a, "shape"_a = 1, doc_empty)
     .def("empty",
          [](nb::type_object dtype, std::vector<size_t> shape) {
              return full(dtype, nb::handle(), shape);
          }, "dtype"_a, "shape"_a)
     .def("zeros",
          [](nb::type_object dtype, size_t size) {
              return full(dtype, nb::int_(0), size);
          }, "dtype"_a, "shape"_a = 1, doc_zeros)
     .def("zeros",
          [](nb::type_object dtype, std::vector<size_t> shape) {
              return full(dtype, nb::int_(0), shape);
          }, "dtype"_a, "shape"_a)
     .def("ones",
          [](nb::type_object dtype, size_t size) {
              return full(dtype, nb::int_(1), size);
          }, "dtype"_a, "shape"_a = 1, doc_ones)
     .def("ones",
          [](nb::type_object dtype, std::vector<size_t> shape) {
              return full(dtype, nb::int_(1), shape);
          }, "dtype"_a, "shape"_a)
     .def("full",
          [](nb::type_object dtype, nb::handle value, size_t size) {
              return full(dtype, value, size);
          }, "dtype"_a, "value"_a, "shape"_a = 1, doc_full)
     .def("full",
          [](nb::type_object dtype, nb::handle value, std::vector<size_t> shape) {
              return full(dtype, value, shape);
          }, "dtype"_a, "value"_a, "shape"_a)
     .def("arange",
          [](const nb::type_object_t<dr::ArrayBase> &dtype, Py_ssize_t size) {
              return arange(dtype, 0, size, 1);
          }, "dtype"_a, "size"_a, doc_arange)
     .def("arange",
          [](const nb::type_object_t<dr::ArrayBase> &dtype,
             Py_ssize_t start, Py_ssize_t stop, Py_ssize_t step) {
              return arange(dtype, start, stop, step);
        }, "dtype"_a, "start"_a, "stop"_a, "step"_a = 1)
     .def("linspace",
          [](const nb::type_object_t<dr::ArrayBase> &dtype, double start,
             double stop, size_t num, bool endpoint) {
              return linspace(dtype, start, stop, num, endpoint);
          }, "dtype"_a, "start"_a, "stop"_a, "num"_a,
             "endpoint"_a = true, doc_linspace);
}