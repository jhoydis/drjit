/*
    switch.cpp -- implementation of drjit.switch() and drjit.dispatch()

    Dr.Jit: A Just-In-Time-Compiler for Differentiable Rendering
    Copyright 2023, Realistic Graphics Lab, EPFL.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include "switch.h"
#include "base.h"
#include "misc.h"

struct dr_index_vector : dr::dr_vector<uint64_t> {
    using Base = dr::dr_vector<uint64_t>;
    using Base::Base;
    ~dr_index_vector() {
        for (size_t i = 0; i < size(); ++i)
            ad_var_dec_ref(operator[](i));
    }
};

/// Exctract the mask parameter from a set of positional/keyword arguments
static nb::object extract_mask(nb::list &args, nb::kwargs &kwargs) {
    nb::str mask_key("active");
    nb::object mask;
    size_t argc = args.size();

    if (kwargs.contains(mask_key)) {
        mask = kwargs[mask_key];
        nb::del(kwargs[mask_key]);
        kwargs[mask_key] = mask.type()(true);
    } else if (argc > 0) {
        nb::object last = args[argc - 1];
        nb::handle last_tp = last.type();
        bool is_mask = false;

        if (is_drjit_type(last_tp)) {
            const ArraySupplement &s = supp(last_tp);
            if ((JitBackend) s.backend != JitBackend::None && s.ndim == 1 &&
                (VarType) s.type == VarType::Bool) {
                is_mask = true;
            }
        } else if (last_tp.is(&PyBool_Type)) {
            is_mask = true;
        }

        if (is_mask) {
            mask = last;
            nb::del(args[argc-1]);
            args.append(mask.type()(true));
        }
    }
    return mask;
}


nb::object switch_impl(nb::handle index_, nb::sequence callables,
                       nb::args args_, nb::kwargs kwargs) {
    struct State {
        nb::tuple args_o;
        nb::object callables_o;
        nb::object rv_o;

        ~State() {
            if (!nb::is_alive())
                return;
            nb::gil_scoped_acquire guard;
            args_o.reset();
            callables_o.reset();
            rv_o.reset();
        }
    };

    try {
        nb::list args(args_);
        nb::object mask = extract_mask(args, kwargs);

        nb::handle index_tp = index_.type();
        if (index_tp.is(&PyLong_Type)) {
            if (mask.is_valid()) {
                bool mask_b = false;
                raise_if(!nb::try_cast(mask, mask_b),
                         "the provided 'mask' argument must be scalar if "
                         "'index' is scalar");
                if (!mask_b)
                    return nb::none();
            }

            return callables[index_](*args, **kwargs);
        }

        // Shift the callable index (ad_call interprets 0 as 'disabled')
        nb::object index = index_ + nb::int_(1);
        index_tp = index.type();

        raise_if(!is_drjit_type(index_tp),
                 "the 'index' argument must be a Dr.Jit array");

        const ArraySupplement &s = supp(index_tp);
        raise_if((JitBackend) s.backend == JitBackend::None ||
                     (VarType) s.type != VarType::UInt32 || s.ndim != 1,
                 "the 'index' argument must be a Jit-compiled 1D 32-bit "
                 "unsigned integer array");

        ad_call_func func = [](void *ptr, void *self,
                                   const dr::dr_vector<uint64_t> &args_i,
                                   dr::dr_vector<uint64_t> &rv_i) {
            nb::gil_scoped_acquire guard;
            State &state = *(State *) ptr;
            state.args_o =
                nb::borrow<nb::tuple>(update_indices(state.args_o, args_i));

            uintptr_t index = (uintptr_t) self;
            nb::object result =
                state.callables_o[index](*state.args_o[0], **state.args_o[1]);

            if (state.rv_o.is_valid())
                check_compatibility(result, state.rv_o);

            state.rv_o = std::move(result);
            collect_indices(state.rv_o, rv_i);
        };

        State *state =
            new State{ nb::make_tuple(args, kwargs), callables, nb::object() };
        ad_call_cleanup cleanup = [](void *ptr) { delete (State *) ptr; };

        dr_vector<uint64_t> args_i;
        dr_index_vector rv_i;
        collect_indices(state->args_o, args_i);

        bool done = ad_call(
            (JitBackend) s.backend, nullptr, nb::len(callables), "drjit.switch()", false,
            (uint32_t) s.index(inst_ptr(index)),
            mask.is_valid() ? ((uint32_t) s.index(inst_ptr(mask))) : 0u, args_i,
            rv_i, state, func, cleanup, true);

        nb::object result = update_indices(state->rv_o, rv_i);

        if (done)
            cleanup(state);

        return result;
    } catch (nb::python_error &e) {
        nb::raise_from(e, PyExc_RuntimeError,
                       "drjit.switch(): encountered an exception (see above).");
    } catch (const std::exception &e) {
        nb::chain_error(PyExc_RuntimeError, "drjit.switch(): %s!", e.what());
        nb::raise_python_error();
    }
}

nb::object dispatch_impl(nb::handle_t<dr::ArrayBase> instances,
                         nb::callable callable, nb::args args_,
                         nb::kwargs kwargs) {
    struct State {
        const std::type_info *type;
        nb::tuple args_o;
        nb::object callable_o;
        nb::object rv_o;

        ~State() {
            if (!nb::is_alive())
                return;
            nb::gil_scoped_acquire guard;
            args_o.reset();
            callable_o.reset();
            rv_o.reset();
        }
    };

    const ArraySupplement &s = supp(instances.type());
    if (!s.is_class || s.ndim != 1)
        nb::raise("drjit.dispatch(): 'instances' parameter must be an instance array.");

    nb::object domain_name = nb::getattr(instances.type(), "Domain", nb::handle());
    if (!domain_name.is_valid() || !nb::isinstance<nb::str>(domain_name))
        nb::raise("drjit.dispatch(): The instance array type ('%s') lacks the "
                  "'Domain' name attribute.", nb::type_name(instances.type()).c_str());

    try {
        nb::list args(args_);
        nb::object mask = extract_mask(args, kwargs);

        ad_call_func func = [](void *ptr, void *self,
                               const dr::dr_vector<uint64_t> &args_i,
                               dr::dr_vector<uint64_t> &rv_i) {
            nb::gil_scoped_acquire guard;
            State &state = *(State *) ptr;
            state.args_o =
                nb::borrow<nb::tuple>(update_indices(state.args_o, args_i));

            nb::object self_o = nb::steal(nb::detail::nb_type_put(
                state.type, self, nb::rv_policy::reference, nullptr));

            nb::object result =
                state.callable_o(self_o, *state.args_o[0], **state.args_o[1]);

            if (state.rv_o.is_valid())
                check_compatibility(result, state.rv_o);

            state.rv_o = std::move(result);
            collect_indices(state.rv_o, rv_i);
        };

        State *state =
            new State{ &nb::type_info(s.value), nb::make_tuple(args, kwargs), callable, nb::object() };
        ad_call_cleanup cleanup = [](void *ptr) { delete (State *) ptr; };

        dr_vector<uint64_t> args_i;
        dr_index_vector rv_i;
        collect_indices(state->args_o, args_i);

        bool done = ad_call(
            (JitBackend) s.backend, nb::borrow<nb::str>(domain_name).c_str(), 0,
            "dispatch()", false, (uint32_t) s.index(inst_ptr(instances)),
            mask.is_valid() ? ((uint32_t) s.index(inst_ptr(mask))) : 0u, args_i,
            rv_i, state, func, cleanup, true);

        nb::object result = update_indices(state->rv_o, rv_i);

        if (done)
            cleanup(state);

        return result;
    } catch (nb::python_error &e) {
        nb::raise_from(e, PyExc_RuntimeError,
                       "drjit.dispatch(): encountered an exception (see above).");
    } catch (const std::exception &e) {
        nb::chain_error(PyExc_RuntimeError, "drjit.dispatch(): %s!", e.what());
        nb::raise_python_error();
    }

    return nb::none();
}

void export_switch(nb::module_&m) {
    m.def("switch", &switch_impl, nb::raw_doc(doc_switch), "index"_a,
          "callables"_a, "args"_a, "kwargs"_a)
     .def("dispatch", &dispatch_impl, doc_dispatch, "instances"_a,
          "callable"_a, "args"_a, "kwargs"_a);
}