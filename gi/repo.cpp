/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <config.h>

#include "repo.h"
#include "ns.h"
#include "function.h"
#include "object.h"
#include "param.h"
#include "boxed.h"
#include "union.h"
#include "enumeration.h"
#include "arg.h"
#include "foreign.h"
#include "fundamental.h"
#include "interface.h"
#include "gerror.h"
#include "cjs/jsapi-class.h"
#include "cjs/jsapi-wrapper.h"
#include "cjs/mem.h"

#include <util/misc.h>

#include <girepository.h>
#include <string.h>

typedef struct {
    void *dummy;

} Repo;

extern struct JSClass gjs_repo_class;

GJS_DEFINE_PRIV_FROM_JS(Repo, gjs_repo_class)

static bool lookup_override_function(JSContext *, JS::HandleId,
                                     JS::MutableHandleValue);

static bool
get_version_for_ns (JSContext       *context,
                    JS::HandleObject repo_obj,
                    JS::HandleId     ns_id,
                    GjsAutoJSChar   *version)
{
    JS::RootedObject versions(context);
    bool found;

    if (!gjs_object_require_property(context, repo_obj,
                                     "GI repository object",
                                     GJS_STRING_GI_VERSIONS, &versions))
        return false;

    if (!JS_AlreadyHasOwnPropertyById(context, versions, ns_id, &found))
        return false;

    if (!found)
        return true;

    return gjs_object_require_property(context, versions, NULL, ns_id, version);
}

static bool
resolve_namespace_object(JSContext       *context,
                         JS::HandleObject repo_obj,
                         JS::HandleId     ns_id,
                         const char      *ns_name)
{
    GIRepository *repo;
    GError *error;

    JSAutoRequest ar(context);

    GjsAutoJSChar version(context);
    if (!get_version_for_ns(context, repo_obj, ns_id, &version))
        return false;

    repo = g_irepository_get_default();
    GList *versions = g_irepository_enumerate_versions(repo, ns_name);
    unsigned nversions = g_list_length(versions);
    if (nversions > 1 && !version &&
        !g_irepository_is_registered(repo, ns_name, NULL)) {
        GjsAutoChar warn_text = g_strdup_printf("Requiring %s but it has %u "
                                                "versions available; use "
                                                "imports.gi.versions to pick one",
                                                ns_name, nversions);
        JS_ReportWarningUTF8(context, "%s", warn_text.get());
    }
    g_list_free_full(versions, g_free);

    error = NULL;
    g_irepository_require(repo, ns_name, version, (GIRepositoryLoadFlags) 0, &error);
    if (error != NULL) {
        gjs_throw(context,
                  "Requiring %s, version %s: %s",
                  ns_name, version?version:"none", error->message);

        g_error_free(error);
        return false;
    }

    /* Defines a property on "obj" (the javascript repo object)
     * with the given namespace name, pointing to that namespace
     * in the repo.
     */
    JS::RootedObject gi_namespace(context, gjs_create_ns(context, ns_name));

    /* Define the property early, to avoid reentrancy issues if
       the override module looks for namespaces that import this */
    if (!JS_DefineProperty(context, repo_obj, ns_name, gi_namespace,
                           GJS_MODULE_PROP_FLAGS))
        g_error("no memory to define ns property");

    JS::RootedValue override(context);
    if (!lookup_override_function(context, ns_id, &override))
        return false;

    JS::RootedValue result(context);
    if (!override.isUndefined() &&
        !JS_CallFunctionValue (context, gi_namespace, /* thisp */
                               override, /* callee */
                               JS::HandleValueArray::empty(), &result))
        return false;

    gjs_debug(GJS_DEBUG_GNAMESPACE,
              "Defined namespace '%s' %p in GIRepository %p", ns_name,
              gi_namespace.get(), repo_obj.get());

    gjs_schedule_gc_if_needed(context);
    return true;
}

/*
 * The *resolved out parameter, on success, should be false to indicate that id
 * was not resolved; and true if id was resolved.
 */
static bool
repo_resolve(JSContext       *context,
             JS::HandleObject obj,
             JS::HandleId     id,
             bool            *resolved)
{
    Repo *priv;

    if (!JSID_IS_STRING(id)) {
        *resolved = false;
        return true; /* not resolved, but no error */
    }

    JSFlatString *str = JSID_TO_FLAT_STRING(id);
    /* let Object.prototype resolve these */
    if (JS_FlatStringEqualsAscii(str, "valueOf") ||
        JS_FlatStringEqualsAscii(str, "toString")) {
        *resolved = false;
        return true;
    }

    GjsAutoJSChar name(context);
    if (!gjs_get_string_id(context, id, &name)) {
        *resolved = false;
        return true;
    }

    priv = priv_from_js(context, obj);
    gjs_debug_jsprop(GJS_DEBUG_GREPO, "Resolve prop '%s' hook obj %p priv %p",
                     name.get(), obj.get(), priv);

    if (priv == NULL) {
        /* we are the prototype, or have the wrong class */
        *resolved = false;
        return true;
    }

    if (!resolve_namespace_object(context, obj, id, name)) {
        return false;
    }

    *resolved = true;
    return true;
}

GJS_NATIVE_CONSTRUCTOR_DEFINE_ABSTRACT(repo)

static void
repo_finalize(JSFreeOp *fop,
              JSObject *obj)
{
    Repo *priv;

    priv = (Repo*) JS_GetPrivate(obj);
    gjs_debug_lifecycle(GJS_DEBUG_GREPO,
                        "finalize, obj %p priv %p", obj, priv);
    if (priv == NULL)
        return; /* we are the prototype, not a real instance */

    GJS_DEC_COUNTER(repo);
    g_slice_free(Repo, priv);
}

/* The bizarre thing about this vtable is that it applies to both
 * instances of the object, and to the prototype that instances of the
 * class have.
 */
static const struct JSClassOps gjs_repo_class_ops = {
    NULL,  /* addProperty */
    NULL,  /* deleteProperty */
    NULL,  /* getProperty */
    NULL,  /* setProperty */
    NULL,  /* enumerate */
    repo_resolve,
    nullptr,  /* mayResolve */
    repo_finalize
};

struct JSClass gjs_repo_class = {
    "GIRepository", /* means "new GIRepository()" works */
    JSCLASS_HAS_PRIVATE | JSCLASS_FOREGROUND_FINALIZE,
    &gjs_repo_class_ops,
};

static JSPropertySpec *gjs_repo_proto_props = nullptr;
static JSFunctionSpec *gjs_repo_proto_funcs = nullptr;
static JSFunctionSpec *gjs_repo_static_funcs = nullptr;

GJS_DEFINE_PROTO_FUNCS(repo)

static JSObject*
repo_new(JSContext *context)
{
    Repo *priv;

    JS::RootedObject proto(context);
    if (!gjs_repo_define_proto(context, nullptr, &proto))
        return nullptr;

    JS::RootedObject repo(context,
        JS_NewObjectWithGivenProto(context, &gjs_repo_class, proto));
    if (repo == nullptr)
        g_error("No memory to create repo object");

    priv = g_slice_new0(Repo);

    GJS_INC_COUNTER(repo);

    g_assert(priv_from_js(context, repo) == NULL);
    JS_SetPrivate(repo, priv);

    gjs_debug_lifecycle(GJS_DEBUG_GREPO,
                        "repo constructor, obj %p priv %p", repo.get(), priv);

    JS::RootedObject versions(context, JS_NewPlainObject(context));
    gjs_object_define_property(context, repo, GJS_STRING_GI_VERSIONS,
                               versions, JSPROP_PERMANENT | JSPROP_RESOLVING);

    /* GLib/GObject/Gio are fixed at 2.0, since we depend on them
     * internally.
     */
    JS::RootedString two_point_oh(context, JS_NewStringCopyZ(context, "2.0"));
    if (!JS_DefineProperty(context, versions, "GLib", two_point_oh,
                           JSPROP_PERMANENT))
        return nullptr;
    if (!JS_DefineProperty(context, versions, "GObject", two_point_oh,
                           JSPROP_PERMANENT))
        return nullptr;
    if (!JS_DefineProperty(context, versions, "Gio", two_point_oh,
                           JSPROP_PERMANENT))
        return nullptr;

    JS::RootedObject private_ns(context, JS_NewPlainObject(context));
    gjs_object_define_property(context, repo,
                               GJS_STRING_PRIVATE_NS_MARKER, private_ns,
                               JSPROP_PERMANENT | JSPROP_RESOLVING);

    return repo;
}

bool
gjs_define_repo(JSContext              *cx,
                JS::MutableHandleObject repo)
{
    repo.set(repo_new(cx));
    return true;
}

static bool
gjs_define_constant(JSContext       *context,
                    JS::HandleObject in_object,
                    GIConstantInfo  *info)
{
    JS::RootedValue value(context);
    GArgument garg = { 0, };
    GITypeInfo *type_info;
    const char *name;
    bool ret = false;

    type_info = g_constant_info_get_type(info);
    g_constant_info_get_value(info, &garg);

    if (!gjs_value_from_g_argument(context, &value, type_info, &garg, true))
        goto out;

    name = g_base_info_get_name((GIBaseInfo*) info);

    if (JS_DefineProperty(context, in_object,
                          name, value,
                          GJS_MODULE_PROP_FLAGS))
        ret = true;

 out:
    g_constant_info_free_value (info, &garg);
    g_base_info_unref((GIBaseInfo*) type_info);
    return ret;
}

#if GJS_VERBOSE_ENABLE_GI_USAGE
void
_gjs_log_info_usage(GIBaseInfo *info)
{
#define DIRECTION_STRING(d) ( ((d) == GI_DIRECTION_IN) ? "IN" : ((d) == GI_DIRECTION_OUT) ? "OUT" : "INOUT" )
#define TRANSFER_STRING(t) ( ((t) == GI_TRANSFER_NOTHING) ? "NOTHING" : ((t) == GI_TRANSFER_CONTAINER) ? "CONTAINER" : "EVERYTHING" )

    {
        char *details;
        GIInfoType info_type;
        GIBaseInfo *container;

        info_type = g_base_info_get_type(info);

        if (info_type == GI_INFO_TYPE_FUNCTION) {
            GString *args;
            int n_args;
            int i;
            GITransfer retval_transfer;

            args = g_string_new("{ ");

            n_args = g_callable_info_get_n_args((GICallableInfo*) info);
            for (i = 0; i < n_args; ++i) {
                GIArgInfo *arg;
                GIDirection direction;
                GITransfer transfer;

                arg = g_callable_info_get_arg((GICallableInfo*)info, i);
                direction = g_arg_info_get_direction(arg);
                transfer = g_arg_info_get_ownership_transfer(arg);

                g_string_append_printf(args,
                                       "{ GI_DIRECTION_%s, GI_TRANSFER_%s }, ",
                                       DIRECTION_STRING(direction),
                                       TRANSFER_STRING(transfer));

                g_base_info_unref((GIBaseInfo*) arg);
            }
            if (args->len > 2)
                g_string_truncate(args, args->len - 2); /* chop comma */

            g_string_append(args, " }");

            retval_transfer = g_callable_info_get_caller_owns((GICallableInfo*) info);

            details = g_strdup_printf(".details = { .func = { .retval_transfer = GI_TRANSFER_%s, .n_args = %d, .args = %s } }",
                                      TRANSFER_STRING(retval_transfer), n_args, args->str);
            g_string_free(args, true);
        } else {
            details = g_strdup_printf(".details = { .nothing = {} }");
        }

        container = g_base_info_get_container(info);

        gjs_debug_gi_usage("{ GI_INFO_TYPE_%s, \"%s\", \"%s\", \"%s\", %s },",
                           gjs_info_type_name(info_type),
                           g_base_info_get_namespace(info),
                           container ? g_base_info_get_name(container) : "",
                           g_base_info_get_name(info),
                           details);
        g_free(details);
    }
}
#endif /* GJS_VERBOSE_ENABLE_GI_USAGE */

bool
gjs_define_info(JSContext       *context,
                JS::HandleObject in_object,
                GIBaseInfo      *info,
                bool            *defined)
{
#if GJS_VERBOSE_ENABLE_GI_USAGE
    _gjs_log_info_usage(info);
#endif

    *defined = true;

    switch (g_base_info_get_type(info)) {
    case GI_INFO_TYPE_FUNCTION:
        {
            JSObject *f;
            f = gjs_define_function(context, in_object, 0, (GICallableInfo*) info);
            if (f == NULL)
                return false;
        }
        break;
    case GI_INFO_TYPE_OBJECT:
        {
            GType gtype;
            gtype = g_registered_type_info_get_g_type((GIRegisteredTypeInfo*)info);

            if (g_type_is_a (gtype, G_TYPE_PARAM)) {
                gjs_define_param_class(context, in_object);
            } else if (g_type_is_a (gtype, G_TYPE_OBJECT)) {
                JS::RootedObject ignored1(context), ignored2(context);
                gjs_define_object_class(context, in_object, info, gtype,
                                        &ignored1, &ignored2);
            } else if (G_TYPE_IS_INSTANTIATABLE(gtype)) {
                JS::RootedObject ignored1(context), ignored2(context);
                if (!gjs_define_fundamental_class(context, in_object,
                                                  (GIObjectInfo*)info,
                                                  &ignored1, &ignored2)) {
                    gjs_throw (context,
                               "Unsupported fundamental class creation for type %s",
                               g_type_name(gtype));
                    return false;
                }
            } else {
                gjs_throw (context,
                           "Unsupported type %s, deriving from fundamental %s",
                           g_type_name(gtype), g_type_name(g_type_fundamental(gtype)));
                return false;
            }
        }
        break;
    case GI_INFO_TYPE_STRUCT:
        /* We don't want GType structures in the namespace,
           we expose their fields as vfuncs and their methods
           as static methods
        */
        if (g_struct_info_is_gtype_struct((GIStructInfo*) info)) {
            *defined = false;
            break;
        }
        /* Fall through */

    case GI_INFO_TYPE_BOXED:
        gjs_define_boxed_class(context, in_object, (GIBoxedInfo*) info);
        break;
    case GI_INFO_TYPE_UNION:
        if (!gjs_define_union_class(context, in_object, (GIUnionInfo*) info))
            return false;
        break;
    case GI_INFO_TYPE_ENUM:
        if (g_enum_info_get_error_domain((GIEnumInfo*) info)) {
            /* define as GError subclass */
            gjs_define_error_class(context, in_object, (GIEnumInfo*) info);
            break;
        }
        /* fall through */

    case GI_INFO_TYPE_FLAGS:
        if (!gjs_define_enumeration(context, in_object, (GIEnumInfo*) info))
            return false;
        break;
    case GI_INFO_TYPE_CONSTANT:
        if (!gjs_define_constant(context, in_object, (GIConstantInfo*) info))
            return false;
        break;
    case GI_INFO_TYPE_INTERFACE:
        {
            JS::RootedObject ignored(context);
            gjs_define_interface_class(context, in_object,
                                       (GIInterfaceInfo *) info,
                                       g_registered_type_info_get_g_type((GIRegisteredTypeInfo *) info),
                                       &ignored);
        }
        break;
    case GI_INFO_TYPE_INVALID:
    case GI_INFO_TYPE_INVALID_0:
    case GI_INFO_TYPE_CALLBACK:
    case GI_INFO_TYPE_VALUE:
    case GI_INFO_TYPE_SIGNAL:
    case GI_INFO_TYPE_VFUNC:
    case GI_INFO_TYPE_PROPERTY:
    case GI_INFO_TYPE_FIELD:
    case GI_INFO_TYPE_ARG:
    case GI_INFO_TYPE_TYPE:
    case GI_INFO_TYPE_UNRESOLVED:
    default:
        gjs_throw(context, "API of type %s not implemented, cannot define %s.%s",
                  gjs_info_type_name(g_base_info_get_type(info)),
                  g_base_info_get_namespace(info),
                  g_base_info_get_name(info));
        return false;
    }

    return true;
}

/* Get the "unknown namespace", which should be used for unnamespaced types */
JSObject*
gjs_lookup_private_namespace(JSContext *context)
{
    JS::HandleId ns_name =
        gjs_context_get_const_string(context, GJS_STRING_PRIVATE_NS_MARKER);
    return gjs_lookup_namespace_object_by_name(context, ns_name);
}

/* Get the namespace object that the GIBaseInfo should be inside */
JSObject*
gjs_lookup_namespace_object(JSContext  *context,
                            GIBaseInfo *info)
{
    const char *ns;

    ns = g_base_info_get_namespace(info);
    if (ns == NULL) {
        gjs_throw(context, "%s '%s' does not have a namespace",
                     gjs_info_type_name(g_base_info_get_type(info)),
                     g_base_info_get_name(info));

        return NULL;
    }

    JS::RootedId ns_name(context, gjs_intern_string_to_id(context, ns));
    return gjs_lookup_namespace_object_by_name(context, ns_name);
}

/* Check if an exception's 'name' property is equal to compare_name. Ignores
 * all errors that might arise. Requires request. */
static bool
error_has_name(JSContext       *cx,
               JS::HandleValue  thrown_value,
               JSString        *compare_name)
{
    if (!thrown_value.isObject())
        return false;

    JS::AutoSaveExceptionState saved_exc(cx);
    JS::RootedObject exc(cx, &thrown_value.toObject());
    JS::RootedValue exc_name(cx);
    bool retval = false;

    if (!gjs_object_get_property(cx, exc, GJS_STRING_NAME, &exc_name))
        goto out;

    int32_t cmp_result;
    if (!JS_CompareStrings(cx, exc_name.toString(), compare_name, &cmp_result))
        goto out;

    if (cmp_result == 0)
        retval = true;

out:
    saved_exc.restore();
    return retval;
}

static bool
lookup_override_function(JSContext             *cx,
                         JS::HandleId           ns_name,
                         JS::MutableHandleValue function)
{
    JSAutoRequest ar(cx);
    JS::AutoSaveExceptionState saved_exc(cx);

    JS::RootedValue importer(cx, gjs_get_global_slot(cx, GJS_GLOBAL_SLOT_IMPORTS));
    g_assert(importer.isObject());

    JS::RootedObject overridespkg(cx), module(cx);
    JS::RootedObject importer_obj(cx, &importer.toObject());

    if (!gjs_object_require_property(cx, importer_obj, "importer",
                                     GJS_STRING_GI_OVERRIDES,
                                     &overridespkg))
        goto fail;

    if (!gjs_object_require_property(cx, overridespkg,
                                     "GI repository object", ns_name,
                                     &module)) {
        JS::RootedValue exc(cx);
        JS_GetPendingException(cx, &exc);

        /* If the exception was an ImportError (i.e., module not found) then
         * we simply didn't have an override, don't throw an exception */
        if (error_has_name(cx, exc, JS_AtomizeAndPinString(cx, "ImportError"))) {
            saved_exc.restore();
            return true;
        }

        goto fail;
    }

    if (!gjs_object_require_property(cx, module, "override module",
                                     GJS_STRING_GOBJECT_INIT, function) ||
        !function.isObjectOrNull()) {
        gjs_throw(cx, "Unexpected value for _init in overrides module");
        goto fail;
    }
    return true;

 fail:
    saved_exc.drop();
    return false;
}

JSObject*
gjs_lookup_namespace_object_by_name(JSContext      *context,
                                    JS::HandleId    ns_name)
{
    JSAutoRequest ar(context);

    JS::RootedValue importer(context,
        gjs_get_global_slot(context, GJS_GLOBAL_SLOT_IMPORTS));
    g_assert(importer.isObject());

    JS::RootedObject repo(context), importer_obj(context, &importer.toObject());

    if (!gjs_object_require_property(context, importer_obj, "importer",
                                     GJS_STRING_GI_MODULE, &repo)) {
        gjs_log_exception(context);
        gjs_throw(context, "No gi property in importer");
        return NULL;
    }

    JS::RootedObject retval(context);
    if (!gjs_object_require_property(context, repo, "GI repository object",
                                     ns_name, &retval))
        return NULL;

    return retval;
}

const char*
gjs_info_type_name(GIInfoType type)
{
    switch (type) {
    case GI_INFO_TYPE_INVALID:
        return "INVALID";
    case GI_INFO_TYPE_FUNCTION:
        return "FUNCTION";
    case GI_INFO_TYPE_CALLBACK:
        return "CALLBACK";
    case GI_INFO_TYPE_STRUCT:
        return "STRUCT";
    case GI_INFO_TYPE_BOXED:
        return "BOXED";
    case GI_INFO_TYPE_ENUM:
        return "ENUM";
    case GI_INFO_TYPE_FLAGS:
        return "FLAGS";
    case GI_INFO_TYPE_OBJECT:
        return "OBJECT";
    case GI_INFO_TYPE_INTERFACE:
        return "INTERFACE";
    case GI_INFO_TYPE_CONSTANT:
        return "CONSTANT";
    case GI_INFO_TYPE_UNION:
        return "UNION";
    case GI_INFO_TYPE_VALUE:
        return "VALUE";
    case GI_INFO_TYPE_SIGNAL:
        return "SIGNAL";
    case GI_INFO_TYPE_VFUNC:
        return "VFUNC";
    case GI_INFO_TYPE_PROPERTY:
        return "PROPERTY";
    case GI_INFO_TYPE_FIELD:
        return "FIELD";
    case GI_INFO_TYPE_ARG:
        return "ARG";
    case GI_INFO_TYPE_TYPE:
        return "TYPE";
    case GI_INFO_TYPE_UNRESOLVED:
        return "UNRESOLVED";
    case GI_INFO_TYPE_INVALID_0:
        g_assert_not_reached();
        break;
    default:
      return "???";
    }
}

char*
gjs_camel_from_hyphen(const char *hyphen_name)
{
    GString *s;
    const char *p;
    bool next_upper;

    s = g_string_sized_new(strlen(hyphen_name) + 1);

    next_upper = false;
    for (p = hyphen_name; *p; ++p) {
        if (*p == '-' || *p == '_') {
            next_upper = true;
        } else {
            if (next_upper) {
                g_string_append_c(s, g_ascii_toupper(*p));
                next_upper = false;
            } else {
                g_string_append_c(s, *p);
            }
        }
    }

    return g_string_free(s, false);
}

char*
gjs_hyphen_from_camel(const char *camel_name)
{
    GString *s;
    const char *p;

    /* four hyphens should be reasonable guess */
    s = g_string_sized_new(strlen(camel_name) + 4 + 1);

    for (p = camel_name; *p; ++p) {
        if (g_ascii_isupper(*p)) {
            g_string_append_c(s, '-');
            g_string_append_c(s, g_ascii_tolower(*p));
        } else {
            g_string_append_c(s, *p);
        }
    }

    return g_string_free(s, false);
}

JSObject *
gjs_lookup_generic_constructor(JSContext  *context,
                               GIBaseInfo *info)
{
    const char *constructor_name;

    JS::RootedObject in_object(context,
        gjs_lookup_namespace_object(context, (GIBaseInfo*) info));
    constructor_name = g_base_info_get_name((GIBaseInfo*) info);

    if (G_UNLIKELY (!in_object))
        return NULL;

    JS::RootedValue value(context);
    if (!JS_GetProperty(context, in_object, constructor_name, &value))
        return NULL;

    if (G_UNLIKELY (!value.isObject()))
        return NULL;

    return &value.toObject();
}

JSObject *
gjs_lookup_generic_prototype(JSContext  *context,
                             GIBaseInfo *info)
{
    JS::RootedObject constructor(context,
                                 gjs_lookup_generic_constructor(context, info));
    if (G_UNLIKELY(!constructor))
        return NULL;

    JS::RootedValue value(context);
    if (!gjs_object_get_property(context, constructor,
                                 GJS_STRING_PROTOTYPE, &value))
        return NULL;

    if (G_UNLIKELY (!value.isObjectOrNull()))
        return NULL;

    return value.toObjectOrNull();
}
