/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Function.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Interpreter.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/GeneratorObjectPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/PromiseReaction.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

ECMAScriptFunctionObject* ECMAScriptFunctionObject::create(GlobalObject& global_object, FlyString name, Statement const& ecmascript_code, Vector<FunctionNode::Parameter> parameters, i32 m_function_length, Environment* parent_scope, PrivateEnvironment* private_scope, FunctionKind kind, bool is_strict, bool might_need_arguments_object, bool contains_direct_call_to_eval, bool is_arrow_function)
{
    Object* prototype = nullptr;
    switch (kind) {
    case FunctionKind::Regular:
        prototype = global_object.function_prototype();
        break;
    case FunctionKind::Generator:
        prototype = global_object.generator_function_prototype();
        break;
    case FunctionKind::Async:
        prototype = global_object.async_function_prototype();
        break;
    }
    return global_object.heap().allocate<ECMAScriptFunctionObject>(global_object, move(name), ecmascript_code, move(parameters), m_function_length, parent_scope, private_scope, *prototype, kind, is_strict, might_need_arguments_object, contains_direct_call_to_eval, is_arrow_function);
}

ECMAScriptFunctionObject::ECMAScriptFunctionObject(FlyString name, Statement const& ecmascript_code, Vector<FunctionNode::Parameter> formal_parameters, i32 function_length, Environment* parent_scope, PrivateEnvironment* private_scope, Object& prototype, FunctionKind kind, bool strict, bool might_need_arguments_object, bool contains_direct_call_to_eval, bool is_arrow_function)
    : FunctionObject(prototype)
    , m_environment(parent_scope)
    , m_private_environment(private_scope)
    , m_formal_parameters(move(formal_parameters))
    , m_ecmascript_code(ecmascript_code)
    , m_realm(global_object().associated_realm())
    , m_strict(strict)
    , m_name(move(name))
    , m_function_length(function_length)
    , m_kind(kind)
    , m_might_need_arguments_object(might_need_arguments_object)
    , m_contains_direct_call_to_eval(contains_direct_call_to_eval)
    , m_is_arrow_function(is_arrow_function)
{
    // NOTE: This logic is from OrdinaryFunctionCreate, https://tc39.es/ecma262/#sec-ordinaryfunctioncreate
    if (m_is_arrow_function)
        m_this_mode = ThisMode::Lexical;
    else if (m_strict)
        m_this_mode = ThisMode::Strict;
    else
        m_this_mode = ThisMode::Global;

    // 15.1.3 Static Semantics: IsSimpleParameterList, https://tc39.es/ecma262/#sec-static-semantics-issimpleparameterlist
    m_has_simple_parameter_list = all_of(m_formal_parameters, [&](auto& parameter) {
        if (parameter.is_rest)
            return false;
        if (parameter.default_value)
            return false;
        if (!parameter.binding.template has<FlyString>())
            return false;
        return true;
    });
}

void ECMAScriptFunctionObject::initialize(GlobalObject& global_object)
{
    auto& vm = this->vm();
    Base::initialize(global_object);
    // Note: The ordering of these properties must be: length, name, prototype which is the order
    //       they are defined in the spec: https://tc39.es/ecma262/#sec-function-instances .
    //       This is observable through something like: https://tc39.es/ecma262/#sec-ordinaryownpropertykeys
    //       which must give the properties in chronological order which in this case is the order they
    //       are defined in the spec.

    MUST(define_property_or_throw(vm.names.length, { .value = Value(m_function_length), .writable = false, .enumerable = false, .configurable = true }));
    MUST(define_property_or_throw(vm.names.name, { .value = js_string(vm, m_name.is_null() ? "" : m_name), .writable = false, .enumerable = false, .configurable = true }));

    if (!m_is_arrow_function) {
        Object* prototype = nullptr;
        switch (m_kind) {
        case FunctionKind::Regular:
            prototype = vm.heap().allocate<Object>(global_object, *global_object.new_ordinary_function_prototype_object_shape());
            MUST(prototype->define_property_or_throw(vm.names.constructor, { .value = this, .writable = true, .enumerable = false, .configurable = true }));
            break;
        case FunctionKind::Generator:
            // prototype is "g1.prototype" in figure-2 (https://tc39.es/ecma262/img/figure-2.png)
            prototype = global_object.generator_object_prototype();
            break;
        case FunctionKind::Async:
            break;
        }
        define_direct_property(vm.names.prototype, prototype, Attribute::Writable);
    }
}

ECMAScriptFunctionObject::~ECMAScriptFunctionObject()
{
}

// 10.2.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-call-thisargument-argumentslist
ThrowCompletionOr<Value> ECMAScriptFunctionObject::internal_call(Value this_argument, MarkedValueList arguments_list)
{
    auto& vm = this->vm();

    // 1. Let callerContext be the running execution context.
    // NOTE: No-op, kept by the VM in its execution context stack.

    ExecutionContext callee_context(heap());

    // Non-standard
    callee_context.arguments.extend(move(arguments_list));
    if (auto* interpreter = vm.interpreter_if_exists())
        callee_context.current_node = interpreter->current_node();

    // 2. Let calleeContext be PrepareForOrdinaryCall(F, undefined).
    prepare_for_ordinary_call(callee_context, nullptr);

    // NOTE: We throw if the end of the native stack is reached, so unlike in the spec this _does_ need an exception check.
    if (auto* exception = vm.exception())
        return throw_completion(exception->value());

    // 3. Assert: calleeContext is now the running execution context.
    VERIFY(&vm.running_execution_context() == &callee_context);

    // 4. If F.[[IsClassConstructor]] is true, then
    if (m_is_class_constructor) {
        // a. Let error be a newly created TypeError object.
        // b. NOTE: error is created in calleeContext with F's associated Realm Record.
        auto throw_completion = vm.throw_completion<TypeError>(global_object(), ErrorType::ClassConstructorWithoutNew, m_name);

        // c. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
        vm.pop_execution_context();

        // d. Return ThrowCompletion(error).
        return throw_completion;
    }

    // 5. Perform OrdinaryCallBindThis(F, calleeContext, thisArgument).
    ordinary_call_bind_this(callee_context, this_argument);

    // 6. Let result be OrdinaryCallEvaluateBody(F, argumentsList).
    auto result = ordinary_call_evaluate_body();

    // 7. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
    vm.pop_execution_context();

    // 8. If result.[[Type]] is return, return NormalCompletion(result.[[Value]]).
    if (result.type() == Completion::Type::Return)
        return result.value();

    // 9. ReturnIfAbrupt(result).
    if (result.is_abrupt()) {
        // NOTE: I'm not sure if EvaluateBody can return a completion other than Normal, Return, or Throw.
        // We're far from using completions in the AST anyway; in the meantime assume Throw.
        VERIFY(result.is_error());
        return result;
    }

    // 10. Return NormalCompletion(undefined).
    return js_undefined();
}

// 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
ThrowCompletionOr<Object*> ECMAScriptFunctionObject::internal_construct(MarkedValueList arguments_list, FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto& global_object = this->global_object();

    // 1. Let callerContext be the running execution context.
    // NOTE: No-op, kept by the VM in its execution context stack.

    // 2. Let kind be F.[[ConstructorKind]].
    auto kind = m_constructor_kind;

    Object* this_argument = nullptr;

    // 3. If kind is base, then
    if (kind == ConstructorKind::Base) {
        // a. Let thisArgument be ? OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
        this_argument = TRY(ordinary_create_from_constructor<Object>(global_object, new_target, &GlobalObject::object_prototype));
    }

    ExecutionContext callee_context(heap());

    // Non-standard
    callee_context.arguments.extend(move(arguments_list));
    if (auto* interpreter = vm.interpreter_if_exists())
        callee_context.current_node = interpreter->current_node();

    // 4. Let calleeContext be PrepareForOrdinaryCall(F, newTarget).
    prepare_for_ordinary_call(callee_context, &new_target);

    // NOTE: We throw if the end of the native stack is reached, so unlike in the spec this _does_ need an exception check.
    if (auto* exception = vm.exception())
        return throw_completion(exception->value());

    // 5. Assert: calleeContext is now the running execution context.
    VERIFY(&vm.running_execution_context() == &callee_context);

    // 6. If kind is base, then
    if (kind == ConstructorKind::Base) {
        // a. Perform OrdinaryCallBindThis(F, calleeContext, thisArgument).
        ordinary_call_bind_this(callee_context, this_argument);

        // b. Let initializeResult be InitializeInstanceElements(thisArgument, F).
        auto initialize_result = vm.initialize_instance_elements(*this_argument, *this);

        // c. If initializeResult is an abrupt completion, then
        if (initialize_result.is_throw_completion()) {
            // i. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
            vm.pop_execution_context();

            // ii. Return Completion(initializeResult).
            return initialize_result.throw_completion();
        }
    }

    // 7. Let constructorEnv be the LexicalEnvironment of calleeContext.
    auto* constructor_env = callee_context.lexical_environment;

    // 8. Let result be OrdinaryCallEvaluateBody(F, argumentsList).
    auto result = ordinary_call_evaluate_body();

    // 9. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
    vm.pop_execution_context();

    // 10. If result.[[Type]] is return, then
    if (result.type() == Completion::Type::Return) {
        // FIXME: This is leftover from untangling the call/construct mess - doesn't belong here in any way, but removing it breaks derived classes.
        // Likely fixed by making ClassDefinitionEvaluation fully spec compliant.
        if (kind == ConstructorKind::Derived && result.value().is_object()) {
            auto prototype = TRY(new_target.get(vm.names.prototype));
            if (prototype.is_object())
                TRY(result.value().as_object().internal_set_prototype_of(&prototype.as_object()));
        }
        // EOF (End of FIXME)

        // a. If Type(result.[[Value]]) is Object, return NormalCompletion(result.[[Value]]).
        if (result.value().is_object())
            return &result.value().as_object();

        // b. If kind is base, return NormalCompletion(thisArgument).
        if (kind == ConstructorKind::Base)
            return this_argument;

        // c. If result.[[Value]] is not undefined, throw a TypeError exception.
        if (!result.value().is_undefined())
            return vm.throw_completion<TypeError>(global_object, ErrorType::DerivedConstructorReturningInvalidValue);
    }
    // 11. Else, ReturnIfAbrupt(result).
    else {
        // NOTE: I'm not sure if EvaluateBody can return a completion other than Normal, Return, or Throw.
        // We're far from using completions in the AST anyway; in the meantime assume Throw.
        VERIFY(result.is_error());
        return result;
    }

    // 12. Return ? constructorEnv.GetThisBinding().
    auto this_binding = TRY(constructor_env->get_this_binding(global_object));
    return &this_binding.as_object();
}

void ECMAScriptFunctionObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_environment);
    visitor.visit(m_realm);
    visitor.visit(m_home_object);

    for (auto& field : m_fields) {
        if (auto* property_name_ptr = field.name.get_pointer<PropertyKey>(); property_name_ptr && property_name_ptr->is_symbol())
            visitor.visit(property_name_ptr->as_symbol());

        visitor.visit(field.initializer);
    }
}

// 10.2.11 FunctionDeclarationInstantiation ( func, argumentsList ), https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
ThrowCompletionOr<void> ECMAScriptFunctionObject::function_declaration_instantiation(Interpreter* interpreter)
{
    auto& vm = this->vm();

    auto& callee_context = vm.running_execution_context();

    // Needed to extract declarations and functions
    ScopeNode const* scope_body = nullptr;
    if (is<ScopeNode>(*m_ecmascript_code))
        scope_body = static_cast<ScopeNode const*>(m_ecmascript_code.ptr());

    bool has_parameter_expressions = false;

    // FIXME: Maybe compute has duplicates at parse time? (We need to anyway since it's an error in some cases)

    bool has_duplicates = false;
    HashTable<FlyString> parameter_names;
    for (auto& parameter : m_formal_parameters) {
        if (parameter.default_value)
            has_parameter_expressions = true;

        parameter.binding.visit(
            [&](FlyString const& name) {
                if (parameter_names.set(name) != AK::HashSetResult::InsertedNewEntry)
                    has_duplicates = true;
            },
            [&](NonnullRefPtr<BindingPattern> const& pattern) {
                if (pattern->contains_expression())
                    has_parameter_expressions = true;

                pattern->for_each_bound_name([&](auto& name) {
                    if (parameter_names.set(name) != AK::HashSetResult::InsertedNewEntry)
                        has_duplicates = true;
                });
            });
    }

    auto arguments_object_needed = m_might_need_arguments_object;

    if (this_mode() == ThisMode::Lexical)
        arguments_object_needed = false;

    if (parameter_names.contains(vm.names.arguments.as_string()))
        arguments_object_needed = false;

    HashTable<FlyString> function_names;
    Vector<FunctionDeclaration const&> functions_to_initialize;

    if (scope_body) {
        scope_body->for_each_var_function_declaration_in_reverse_order([&](FunctionDeclaration const& function) {
            if (function_names.set(function.name()) == AK::HashSetResult::InsertedNewEntry)
                functions_to_initialize.append(function);
        });

        auto const& arguments_name = vm.names.arguments.as_string();

        if (!has_parameter_expressions && function_names.contains(arguments_name))
            arguments_object_needed = false;

        if (!has_parameter_expressions && arguments_object_needed) {
            scope_body->for_each_lexically_declared_name([&](auto const& name) {
                if (name == arguments_name) {
                    arguments_object_needed = false;
                    return IterationDecision::Break;
                }
                return IterationDecision::Continue;
            });
        }
    } else {
        arguments_object_needed = false;
    }

    Environment* environment;

    if (is_strict_mode() || !has_parameter_expressions) {
        environment = callee_context.lexical_environment;
    } else {
        environment = new_declarative_environment(*callee_context.lexical_environment);
        VERIFY(callee_context.variable_environment == callee_context.lexical_environment);
        callee_context.lexical_environment = environment;
    }

    for (auto const& parameter_name : parameter_names) {
        if (MUST(environment->has_binding(parameter_name)))
            continue;

        MUST(environment->create_mutable_binding(global_object(), parameter_name, false));
        if (has_duplicates)
            MUST(environment->initialize_binding(global_object(), parameter_name, js_undefined()));
    }

    if (arguments_object_needed) {
        Object* arguments_object;
        if (is_strict_mode() || !has_simple_parameter_list())
            arguments_object = create_unmapped_arguments_object(global_object(), vm.running_execution_context().arguments);
        else
            arguments_object = create_mapped_arguments_object(global_object(), *this, formal_parameters(), vm.running_execution_context().arguments, *environment);

        if (is_strict_mode())
            MUST(environment->create_immutable_binding(global_object(), vm.names.arguments.as_string(), false));
        else
            MUST(environment->create_mutable_binding(global_object(), vm.names.arguments.as_string(), false));

        MUST(environment->initialize_binding(global_object(), vm.names.arguments.as_string(), arguments_object));
        parameter_names.set(vm.names.arguments.as_string());
    }

    // We now treat parameterBindings as parameterNames.

    // The spec makes an iterator here to do IteratorBindingInitialization but we just do it manually
    auto& execution_context_arguments = vm.running_execution_context().arguments;

    for (size_t i = 0; i < m_formal_parameters.size(); ++i) {
        auto& parameter = m_formal_parameters[i];
        TRY(parameter.binding.visit(
            [&](auto const& param) -> ThrowCompletionOr<void> {
                Value argument_value;
                if (parameter.is_rest) {
                    auto* array = MUST(Array::create(global_object(), 0));
                    for (size_t rest_index = i; rest_index < execution_context_arguments.size(); ++rest_index)
                        array->indexed_properties().append(execution_context_arguments[rest_index]);
                    argument_value = array;
                } else if (i < execution_context_arguments.size() && !execution_context_arguments[i].is_undefined()) {
                    argument_value = execution_context_arguments[i];
                } else if (parameter.default_value) {
                    // FIXME: Support default arguments in the bytecode world!
                    if (interpreter)
                        argument_value = parameter.default_value->execute(*interpreter, global_object());
                    if (auto* exception = vm.exception())
                        return throw_completion(exception->value());
                } else {
                    argument_value = js_undefined();
                }

                Environment* used_environment = has_duplicates ? nullptr : environment;

                if constexpr (IsSame<FlyString const&, decltype(param)>) {
                    Reference reference = vm.resolve_binding(param, used_environment);
                    if (auto* exception = vm.exception())
                        return throw_completion(exception->value());
                    // Here the difference from hasDuplicates is important
                    if (has_duplicates)
                        return reference.put_value(global_object(), argument_value);
                    else
                        return reference.initialize_referenced_binding(global_object(), argument_value);
                } else if (IsSame<NonnullRefPtr<BindingPattern> const&, decltype(param)>) {
                    // Here the difference from hasDuplicates is important
                    return vm.binding_initialization(param, argument_value, used_environment, global_object());
                }
            }));
    }

    Environment* var_environment;

    HashTable<FlyString> instantiated_var_names;
    if (scope_body)
        instantiated_var_names.ensure_capacity(scope_body->var_declaration_count());

    if (!has_parameter_expressions) {
        if (scope_body) {
            scope_body->for_each_var_declared_name([&](auto const& name) {
                if (!parameter_names.contains(name) && instantiated_var_names.set(name) == AK::HashSetResult::InsertedNewEntry) {
                    MUST(environment->create_mutable_binding(global_object(), name, false));
                    MUST(environment->initialize_binding(global_object(), name, js_undefined()));
                }
            });
        }
        var_environment = environment;
    } else {
        var_environment = new_declarative_environment(*environment);
        callee_context.variable_environment = var_environment;

        if (scope_body) {
            scope_body->for_each_var_declared_name([&](auto const& name) {
                if (instantiated_var_names.set(name) != AK::HashSetResult::InsertedNewEntry)
                    return IterationDecision::Continue;
                MUST(var_environment->create_mutable_binding(global_object(), name, false));

                Value initial_value;
                if (!parameter_names.contains(name) || function_names.contains(name))
                    initial_value = js_undefined();
                else
                    initial_value = MUST(environment->get_binding_value(global_object(), name, false));

                MUST(var_environment->initialize_binding(global_object(), name, initial_value));

                return IterationDecision::Continue;
            });
        }
    }

    // B.3.2.1 Changes to FunctionDeclarationInstantiation, https://tc39.es/ecma262/#sec-web-compat-functiondeclarationinstantiation
    if (!m_strict && scope_body) {
        scope_body->for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) {
            auto& function_name = function_declaration.name();
            if (parameter_names.contains(function_name))
                return IterationDecision::Continue;
            // The spec says 'initializedBindings' here but that does not exist and it then adds it to 'instantiatedVarNames' so it probably means 'instantiatedVarNames'.
            if (!instantiated_var_names.contains(function_name) && function_name != vm.names.arguments.as_string()) {
                MUST(var_environment->create_mutable_binding(global_object(), function_name, false));
                MUST(var_environment->initialize_binding(global_object(), function_name, js_undefined()));
                instantiated_var_names.set(function_name);
            }

            function_declaration.set_should_do_additional_annexB_steps();
            return IterationDecision::Continue;
        });
    }

    Environment* lex_environment;

    // 30. If strict is false, then
    if (!is_strict_mode()) {
        // Optimization: We avoid creating empty top-level declarative environments in non-strict mode, if both of these conditions are true:
        //               1. there is no direct call to eval() within this function
        //               2. there are no lexical declarations that would go into the environment
        bool can_elide_declarative_environment = !m_contains_direct_call_to_eval && (!scope_body || !scope_body->has_lexical_declarations());
        if (can_elide_declarative_environment) {
            lex_environment = var_environment;
        } else {
            // a. Let lexEnv be NewDeclarativeEnvironment(varEnv).
            // b. NOTE: Non-strict functions use a separate Environment Record for top-level lexical declarations so that a direct eval
            //          can determine whether any var scoped declarations introduced by the eval code conflict with pre-existing top-level
            //          lexically scoped declarations. This is not needed for strict functions because a strict direct eval always places
            //          all declarations into a new Environment Record.
            lex_environment = new_declarative_environment(*var_environment);
        }
    } else {
        // 31. Else, let lexEnv be varEnv.
        lex_environment = var_environment;
    }

    // 32. Set the LexicalEnvironment of calleeContext to lexEnv.
    callee_context.lexical_environment = lex_environment;

    if (!scope_body)
        return {};

    scope_body->for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        declaration.for_each_bound_name([&](auto const& name) {
            if (declaration.is_constant_declaration())
                MUST(lex_environment->create_immutable_binding(global_object(), name, true));
            else
                MUST(lex_environment->create_mutable_binding(global_object(), name, false));
            return IterationDecision::Continue;
        });
    });

    VERIFY(!vm.exception());
    auto* private_environment = callee_context.private_environment;
    for (auto& declaration : functions_to_initialize) {
        auto* function = ECMAScriptFunctionObject::create(global_object(), declaration.name(), declaration.body(), declaration.parameters(), declaration.function_length(), lex_environment, private_environment, declaration.kind(), declaration.is_strict_mode(), declaration.might_need_arguments_object(), declaration.contains_direct_call_to_eval());
        MUST(var_environment->set_mutable_binding(global_object(), declaration.name(), function, false));
    }

    return {};
}

// 10.2.1.1 PrepareForOrdinaryCall ( F, newTarget ), https://tc39.es/ecma262/#sec-prepareforordinarycall
void ECMAScriptFunctionObject::prepare_for_ordinary_call(ExecutionContext& callee_context, Object* new_target)
{
    auto& vm = this->vm();

    // Non-standard
    callee_context.is_strict_mode = m_strict;

    // 1. Let callerContext be the running execution context.
    // 2. Let calleeContext be a new ECMAScript code execution context.

    // NOTE: In the specification, PrepareForOrdinaryCall "returns" a new callee execution context.
    // To avoid heap allocations, we put our ExecutionContext objects on the C++ stack instead.
    // Whoever calls us should put an ExecutionContext on their stack and pass that as the `callee_context`.

    // 3. Set the Function of calleeContext to F.
    callee_context.function = this;
    callee_context.function_name = m_name;

    // 4. Let calleeRealm be F.[[Realm]].
    auto* callee_realm = m_realm;
    // NOTE: This non-standard fallback is needed until we can guarantee that literally
    // every function has a realm - especially in LibWeb that's sometimes not the case
    // when a function is created while no JS is running, as we currently need to rely on
    // that (:acid2:, I know - see set_event_handler_attribute() for an example).
    // If there's no 'current realm' either, we can't continue and crash.
    if (!callee_realm)
        callee_realm = vm.current_realm();
    VERIFY(callee_realm);

    // 5. Set the Realm of calleeContext to calleeRealm.
    callee_context.realm = callee_realm;

    // 6. Set the ScriptOrModule of calleeContext to F.[[ScriptOrModule]].
    // FIXME: Our execution context struct currently does not track this item.

    // 7. Let localEnv be NewFunctionEnvironment(F, newTarget).
    auto* local_environment = new_function_environment(*this, new_target);

    // 8. Set the LexicalEnvironment of calleeContext to localEnv.
    callee_context.lexical_environment = local_environment;

    // 9. Set the VariableEnvironment of calleeContext to localEnv.
    callee_context.variable_environment = local_environment;

    // 10. Set the PrivateEnvironment of calleeContext to F.[[PrivateEnvironment]].
    callee_context.private_environment = m_private_environment;

    // 11. If callerContext is not already suspended, suspend callerContext.
    // FIXME: We don't have this concept yet.

    // 12. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.
    vm.push_execution_context(callee_context, global_object());

    // 13. NOTE: Any exception objects produced after this point are associated with calleeRealm.
    // 14. Return calleeContext. (See NOTE above about how contexts are allocated on the C++ stack.)
}

// 10.2.1.2 OrdinaryCallBindThis ( F, calleeContext, thisArgument ), https://tc39.es/ecma262/#sec-ordinarycallbindthis
void ECMAScriptFunctionObject::ordinary_call_bind_this(ExecutionContext& callee_context, Value this_argument)
{
    auto& vm = this->vm();

    // 1. Let thisMode be F.[[ThisMode]].
    auto this_mode = m_this_mode;

    // If thisMode is lexical, return NormalCompletion(undefined).
    if (this_mode == ThisMode::Lexical)
        return;

    // 3. Let calleeRealm be F.[[Realm]].
    auto* callee_realm = m_realm;
    // NOTE: This non-standard fallback is needed until we can guarantee that literally
    // every function has a realm - especially in LibWeb that's sometimes not the case
    // when a function is created while no JS is running, as we currently need to rely on
    // that (:acid2:, I know - see set_event_handler_attribute() for an example).
    // If there's no 'current realm' either, we can't continue and crash.
    if (!callee_realm)
        callee_realm = vm.current_realm();
    VERIFY(callee_realm);

    // 4. Let localEnv be the LexicalEnvironment of calleeContext.
    auto* local_env = callee_context.lexical_environment;

    Value this_value;

    // 5. If thisMode is strict, let thisValue be thisArgument.
    if (this_mode == ThisMode::Strict) {
        this_value = this_argument;
    }
    // 6. Else,
    else {
        // a. If thisArgument is undefined or null, then
        if (this_argument.is_nullish()) {
            // i. Let globalEnv be calleeRealm.[[GlobalEnv]].
            // ii. Assert: globalEnv is a global Environment Record.
            auto& global_env = callee_realm->global_environment();

            // iii. Let thisValue be globalEnv.[[GlobalThisValue]].
            this_value = &global_env.global_this_value();
        }
        // b. Else,
        else {
            // i. Let thisValue be ! ToObject(thisArgument).
            this_value = MUST(this_argument.to_object(global_object()));

            // ii. NOTE: ToObject produces wrapper objects using calleeRealm.
            // FIXME: It currently doesn't, as we pass the function's global object.
        }
    }

    // 7. Assert: localEnv is a function Environment Record.
    // 8. Assert: The next step never returns an abrupt completion because localEnv.[[ThisBindingStatus]] is not initialized.
    // 9. Return localEnv.BindThisValue(thisValue).
    MUST(verify_cast<FunctionEnvironment>(local_env)->bind_this_value(global_object(), this_value));
}

// 27.7.5.1 AsyncFunctionStart ( promiseCapability, asyncFunctionBody ), https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
void ECMAScriptFunctionObject::async_function_start(PromiseCapability const& promise_capability)
{
    auto& vm = this->vm();

    // 1. Let runningContext be the running execution context.
    auto& running_context = vm.running_execution_context();

    // 2. Let asyncContext be a copy of runningContext.
    auto async_context = running_context.copy();

    // 3. NOTE: Copying the execution state is required for AsyncBlockStart to resume its execution. It is ill-defined to resume a currently executing context.

    // 4. Perform ! AsyncBlockStart(promiseCapability, asyncFunctionBody, asyncContext).
    async_block_start(promise_capability, async_context);
}

// 27.7.5.2 AsyncBlockStart ( promiseCapability, asyncBody, asyncContext ), https://tc39.es/ecma262/#sec-asyncblockstart
void ECMAScriptFunctionObject::async_block_start(PromiseCapability const& promise_capability, ExecutionContext& async_context)
{
    auto& vm = this->vm();

    // 1. Assert: promiseCapability is a PromiseCapability Record.

    // 2. Let runningContext be the running execution context.
    auto& running_context = vm.running_execution_context();

    // 3. Set the code evaluation state of asyncContext such that when evaluation is resumed for that execution context the following steps will be performed:
    auto* execution_steps = NativeFunction::create(global_object(), "", [async_body = m_ecmascript_code, &promise_capability](auto& vm, auto& global_object) -> ThrowCompletionOr<Value> {
        // a. Let result be the result of evaluating asyncBody.
        auto result = async_body->execute(vm.interpreter(), global_object);

        // b. Assert: If we return here, the async function either threw an exception or performed an implicit or explicit return; all awaiting is done.

        // c. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the execution context stack as the running execution context.
        vm.pop_execution_context();

        // NOTE: Running the AST node should eventually return a completion.
        // Until it does, we assume "return" and include the undefined fallback from the call site.
        // d. If result.[[Type]] is normal, then
        if (false) {
            // i. Perform ! Call(promiseCapability.[[Resolve]], undefined, « undefined »).
            MUST(call(global_object, promise_capability.resolve, js_undefined(), js_undefined()));
        }
        // e. Else if result.[[Type]] is return, then
        else if (!vm.exception()) {
            // i. Perform ! Call(promiseCapability.[[Resolve]], undefined, « result.[[Value]] »).
            MUST(call(global_object, promise_capability.resolve, js_undefined(), result.value_or(js_undefined())));
        }
        // f. Else,
        else {
            // i. Assert: result.[[Type]] is throw.

            // ii. Perform ! Call(promiseCapability.[[Reject]], undefined, « result.[[Value]] »).
            auto reason = vm.exception()->value();
            vm.clear_exception();
            vm.stop_unwind();
            MUST(call(global_object, promise_capability.reject, js_undefined(), reason));
        }
        // g. Return.
        return js_undefined();
    });

    // 4. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
    vm.push_execution_context(async_context, global_object());

    // 5. Resume the suspended evaluation of asyncContext. Let result be the value returned by the resumed computation.
    auto result = vm.call(*execution_steps, async_context.this_value.is_empty() ? js_undefined() : async_context.this_value);

    // 6. Assert: When we return here, asyncContext has already been removed from the execution context stack and runningContext is the currently running execution context.
    VERIFY(&vm.running_execution_context() == &running_context);

    // 7. Assert: result is a normal completion with a value of undefined. The possible sources of completion values are Await or, if the async function doesn't await anything, step 3.g above.
    VERIFY(result.has_value() && result.value().is_undefined());

    // 8. Return.
}

// 10.2.1.4 OrdinaryCallEvaluateBody ( F, argumentsList ), https://tc39.es/ecma262/#sec-ordinarycallevaluatebody
Completion ECMAScriptFunctionObject::ordinary_call_evaluate_body()
{
    auto& vm = this->vm();
    auto* bytecode_interpreter = Bytecode::Interpreter::current();

    if (bytecode_interpreter) {
        if (m_kind == FunctionKind::Async)
            return vm.throw_completion<InternalError>(global_object(), ErrorType::NotImplemented, "Async function execution in Bytecode interpreter");
        // FIXME: pass something to evaluate default arguments with
        TRY(function_declaration_instantiation(nullptr));
        if (!m_bytecode_executable.has_value()) {
            m_bytecode_executable = Bytecode::Generator::generate(m_ecmascript_code, m_kind == FunctionKind::Generator);
            m_bytecode_executable->name = m_name;
            auto& passes = JS::Bytecode::Interpreter::optimization_pipeline();
            passes.perform(*m_bytecode_executable);
            if constexpr (JS_BYTECODE_DEBUG) {
                dbgln("Optimisation passes took {}us", passes.elapsed());
                dbgln("Compiled Bytecode::Block for function '{}':", m_name);
            }
            if (JS::Bytecode::g_dump_bytecode)
                m_bytecode_executable->dump();
        }
        auto result = bytecode_interpreter->run(*m_bytecode_executable);
        if (auto* exception = vm.exception())
            return throw_completion(exception->value());
        // NOTE: Running the bytecode should eventually return a completion.
        // Until it does, we assume "return" and include the undefined fallback from the call site.
        if (m_kind != FunctionKind::Generator)
            return { Completion::Type::Return, result.value_or(js_undefined()), {} };

        return normal_completion(TRY(GeneratorObject::create(global_object(), result, this, vm.running_execution_context().lexical_environment, bytecode_interpreter->snapshot_frame())));
    } else {
        if (m_kind == FunctionKind::Generator)
            return vm.throw_completion<InternalError>(global_object(), ErrorType::NotImplemented, "Generator function execution in AST interpreter");
        OwnPtr<Interpreter> local_interpreter;
        Interpreter* ast_interpreter = vm.interpreter_if_exists();

        if (!ast_interpreter) {
            local_interpreter = Interpreter::create_with_existing_realm(*realm());
            ast_interpreter = local_interpreter.ptr();
        }

        VM::InterpreterExecutionScope scope(*ast_interpreter);

        if (m_kind == FunctionKind::Regular) {
            TRY(function_declaration_instantiation(ast_interpreter));

            auto result = m_ecmascript_code->execute(*ast_interpreter, global_object());
            if (auto* exception = vm.exception())
                return throw_completion(exception->value());
            // NOTE: Running the AST node should eventually return a completion.
            // Until it does, we assume "return" and include the undefined fallback from the call site.
            return { Completion::Type::Return, result.value_or(js_undefined()), {} };
        } else if (m_kind == FunctionKind::Async) {
            // 1. Let promiseCapability be ! NewPromiseCapability(%Promise%).
            auto promise_capability = MUST(new_promise_capability(global_object(), global_object().promise_constructor()));

            // 2. Let declResult be FunctionDeclarationInstantiation(functionObject, argumentsList).
            auto declaration_result = function_declaration_instantiation(ast_interpreter);

            // 3. If declResult is not an abrupt completion, then
            if (!declaration_result.is_throw_completion() || !declaration_result.throw_completion().is_abrupt()) {
                // a. Perform ! AsyncFunctionStart(promiseCapability, FunctionBody).
                async_function_start(promise_capability);
            }
            // 4. Else,
            else {
                // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « declResult.[[Value]] »).
                MUST(call(global_object(), promise_capability.reject, js_undefined(), declaration_result.throw_completion().value()));
            }

            // 5. Return Completion { [[Type]]: return, [[Value]]: promiseCapability.[[Promise]], [[Target]]: empty }.
            return Completion { Completion::Type::Return, promise_capability.promise, {} };
        }
    }
    VERIFY_NOT_REACHED();
}

void ECMAScriptFunctionObject::set_name(const FlyString& name)
{
    VERIFY(!name.is_null());
    auto& vm = this->vm();
    m_name = name;
    auto success = MUST(define_property_or_throw(vm.names.name, { .value = js_string(vm, m_name), .writable = false, .enumerable = false, .configurable = true }));
    VERIFY(success);
}

void ECMAScriptFunctionObject::add_field(ClassElement::ClassElementName property_key, ECMAScriptFunctionObject* initializer)
{
    m_fields.empend(property_key, initializer);
}

}
