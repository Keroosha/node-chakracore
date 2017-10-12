//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/JSONStack.h"
#include "Library/JSONParser.h"
#include "Library/JSON.h"

#define MAX_JSON_STRINGIFY_NAMES_ON_STACK 20
static const int JSONspaceSize = 10; //ES5 defined limit on the indentation space
using namespace Js;


namespace JSON
{
    Js::FunctionInfo EntryInfo::Stringify(FORCE_NO_WRITE_BARRIER_TAG(JSON::Stringify), Js::FunctionInfo::ErrorOnNew);
    Js::FunctionInfo EntryInfo::Parse(FORCE_NO_WRITE_BARRIER_TAG(JSON::Parse), Js::FunctionInfo::ErrorOnNew);

    Js::Var Parse(Js::JavascriptString* input, Js::RecyclableObject* reviver, Js::ScriptContext* scriptContext);

    Js::Var Parse(Js::RecyclableObject* function, Js::CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        //ES5:  parse(text [, reviver])
        ARGUMENTS(args, callInfo);
        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Js::ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("JSON.parse"));
        Assert(!(callInfo.Flags & Js::CallFlags_New));

        if(args.Info.Count < 2)
        {
            // if the text argument is missing it is assumed to be undefined.
            // ToString(undefined) returns "undefined" which is not a JSON grammar correct construct.  Shortcut and throw here
            Js::JavascriptError::ThrowSyntaxError(scriptContext, ERRsyntax);
        }

        Js::JavascriptString* input;
        Js::Var value = args[1];
        if (Js::JavascriptString::Is(value))
        {
            input = Js::JavascriptString::FromVar(value);
        }
        else
        {
            input = Js::JavascriptConversion::ToString(value, scriptContext);
        }
        Js::RecyclableObject* reviver = NULL;
        if (args.Info.Count > 2 && Js::JavascriptConversion::IsCallable(args[2]))
        {
            reviver = Js::RecyclableObject::FromVar(args[2]);
        }

        return Parse(input, reviver, scriptContext);
    }

    Js::Var Parse(Js::JavascriptString* input, Js::RecyclableObject* reviver, Js::ScriptContext* scriptContext)
    {
        // alignment required because of the union in JSONParser::m_token
        __declspec (align(8)) JSONParser parser(scriptContext, reviver);
        Js::Var result = NULL;

        TryFinally([&]()
        {
            result = parser.Parse(input);

#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
            if (CONFIG_FLAG(ForceGCAfterJSONParse))
            {
                Recycler* recycler = scriptContext->GetRecycler();
                recycler->CollectNow<CollectNowForceInThread>();
            }
#endif

            if(reviver)
            {
                Js::DynamicObject* root = scriptContext->GetLibrary()->CreateObject();
                JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(root));
                Js::PropertyId propertyId = scriptContext->GetEmptyStringPropertyId();
                Js::JavascriptOperators::InitProperty(root, propertyId, result);
                result = parser.Walk(scriptContext->GetLibrary()->GetEmptyString(), propertyId, root);
            }
        },
        [&](bool/*hasException*/)
        {
            parser.Finalizer();
        });

        return result;
    }

    inline bool IsValidReplacerType(Js::TypeId typeId)
    {
        switch(typeId)
        {
            case Js::TypeIds_Integer:
            case Js::TypeIds_String:
            case Js::TypeIds_Number:
            case Js::TypeIds_NumberObject:
            case Js::TypeIds_Int64Number:
            case Js::TypeIds_UInt64Number:
            case Js::TypeIds_StringObject:
                return true;
        }
        return false;
    }

    uint32 AddToNameTable(StringifySession::StringTable nameTable[], uint32 tableLen, uint32 size, Js::Var item, Js::ScriptContext* scriptContext)
    {
        Js::Var value = nullptr;
        switch (Js::JavascriptOperators::GetTypeId(item))
        {
        case Js::TypeIds_Integer:
            value = scriptContext->GetIntegerString(item);
            break;
        case Js::TypeIds_String:
            value = item;
            break;
        case Js::TypeIds_Number:
        case Js::TypeIds_NumberObject:
        case Js::TypeIds_Int64Number:
        case Js::TypeIds_UInt64Number:
        case Js::TypeIds_StringObject:
            value = Js::JavascriptConversion::ToString(item, scriptContext);
            break;
        }
        if (value && Js::JavascriptString::Is(value))
        {
            // Only validate size when about to modify it. We skip over all other (non-valid) replacement elements.
            if (tableLen == size)
            {
                Js::Throw::FatalInternalError(); // nameTable buffer calculation is wrong
            }
            Js::JavascriptString *propertyName = Js::JavascriptString::FromVar(value);
            nameTable[tableLen].propName = propertyName;
            Js::PropertyRecord const * propertyRecord;
            scriptContext->GetOrAddPropertyRecord(propertyName, &propertyRecord);
            nameTable[tableLen].propRecord = propertyRecord;        // Keep the property id alive.
            tableLen++;
        }
        return tableLen;
    }

    BVSparse<ArenaAllocator>* AllocateMap(ArenaAllocator *tempAlloc)
    {
        //To escape error C2712: Cannot use __try in functions that require object unwinding
        return Anew(tempAlloc, BVSparse<ArenaAllocator>, tempAlloc);
    }

    Js::Var Stringify(Js::RecyclableObject* function, Js::CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        //ES5: Stringify(value, [replacer][, space]])
        ARGUMENTS(args, callInfo);
        Js::JavascriptLibrary* library = function->GetType()->GetLibrary();
        Js::ScriptContext* scriptContext = library->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("JSON.stringify"));

        Assert(!(callInfo.Flags & Js::CallFlags_New));

        if (args.Info.Count < 2)
        {
            // if value is missing it is assumed to be 'undefined'.
            // shortcut: the stringify algorithm returns undefined in this case.
            return library->GetUndefined();
        }

        Js::Var value = args[1];
        Js::Var replacerArg = args.Info.Count > 2 ? args[2] : nullptr;
        Js::Var space = args.Info.Count > 3 ? args[3] : library->GetNull();

        Js::DynamicObject* remoteObject;
        Js::TypeId valueType = Js::JavascriptOperators::GetTypeId(value);
        if (valueType == Js::TypeIds_HostDispatch)
        {
            remoteObject = Js::RecyclableObject::FromVar(value)->GetRemoteObject();
            if (remoteObject != nullptr)
            {
                value = Js::DynamicObject::FromVar(remoteObject);
            }
            else
            {
                Js::Var result;
                if (Js::RecyclableObject::FromVar(value)->InvokeBuiltInOperationRemotely(Stringify, args, &result))
                {
                    return result;
                }
            }
        }

        Js::Var result = nullptr;
        StringifySession stringifySession(scriptContext);
        //stringifySession will point to the memory allocated by nameTable, so make sure lifespans are linked.
        StringifySession::StringTable* nameTable = nullptr;

        DECLARE_TEMP_GUEST_ALLOCATOR(nameTableAlloc);

        if (replacerArg)
        {
            if (Js::JavascriptOperators::IsArray(replacerArg))
            {
                uint32 length;
                Js::JavascriptArray *reArray = nullptr;
                Js::RecyclableObject *reRemoteArray = Js::RecyclableObject::FromVar(replacerArg);
                bool isArray = false;

                if (Js::JavascriptArray::Is(replacerArg))
                {
                    reArray = Js::JavascriptArray::FromVar(replacerArg);
                    length = reArray->GetLength();
                    isArray = true;
                }
                else
                {
                    length = Js::JavascriptConversion::ToUInt32(Js::JavascriptOperators::OP_GetLength(replacerArg, scriptContext), scriptContext);
                }

                uint32 count = 0;
                Js::Var item = nullptr;

                if (isArray)
                {
                    for (uint32 i = 0; i< length; i++)
                    {
                        Js::TypeId idn = Js::JavascriptOperators::GetTypeId(reArray->DirectGetItem(i));
                        if(IsValidReplacerType(idn))
                        {
                            count++;
                        }
                    }
                }
                else
                {
                    for (uint32 i = 0; i< length; i++)
                    {
                        if (Js::JavascriptOperators::GetItem(reRemoteArray, i, &item, scriptContext))
                        {
                            Js::TypeId idn = Js::JavascriptOperators::GetTypeId(item);
                            if(IsValidReplacerType(idn))
                            {
                                count++;
                            }
                        }
                    }
                }

                uint32 tableLen = 0;
                if (count)
                {
                    // the name table goes away with stringify session.
                    if (count < MAX_JSON_STRINGIFY_NAMES_ON_STACK)
                    {
                         PROBE_STACK(scriptContext, (sizeof(StringifySession::StringTable) * count)) ;
                         nameTable = (StringifySession::StringTable*)_alloca(sizeof(StringifySession::StringTable) * count);
                    }
                    else
                    {
                         ACQUIRE_TEMP_GUEST_ALLOCATOR(nameTableAlloc, scriptContext, _u("JSON"));
                         nameTable = AnewArray(nameTableAlloc, StringifySession::StringTable, count);
                    }
                    if (isArray && !!reArray->IsCrossSiteObject())
                    {
                        for (uint32 i = 0; i < length; i++)
                        {
                            item = reArray->DirectGetItem(i);
                            tableLen = AddToNameTable(nameTable, tableLen, count, item, scriptContext);
                        }
                    }
                    else
                    {
                        for (uint32 i = 0; i < length; i++)
                        {
                            if (Js::JavascriptOperators::GetItem(reRemoteArray, i, &item, scriptContext))
                            {
                                tableLen = AddToNameTable(nameTable, tableLen, count, item, scriptContext);
                            }
                        }
                    }

                    //Eliminate duplicates in replacer array.
                    BEGIN_TEMP_ALLOCATOR(tempAlloc, scriptContext, _u("JSON"))
                    {
                        BVSparse<ArenaAllocator>* propIdMap = AllocateMap(tempAlloc); //Anew(tempAlloc, BVSparse<ArenaAllocator>, tempAlloc);

                        // TODO: Potential arithmetic overflow for table size/count/tableLen if large replacement args are specified.
                        // tableLen is ensured by AddToNameTable but this doesn't propagate as an annotation so we assume here to fix the OACR warning.
                        _Analysis_assume_(tableLen <= count);
                        Assert(tableLen <= count);

                        uint32 j = 0;
                        for (uint32 i=0; i < tableLen; i++)
                        {
                            if(propIdMap->TestAndSet(nameTable[i].propRecord->GetPropertyId())) //Find & skip duplicate
                            {
                                continue;
                            }
                            if (j != i)
                            {
                                nameTable[j] = nameTable[i];
                            }
                            j++;
                        }
                        tableLen = j;
                    }
                    END_TEMP_ALLOCATOR(tempAlloc, scriptContext);
                }

                stringifySession.InitReplacer(nameTable, tableLen);
            }
            else if (Js::JavascriptConversion::IsCallable(replacerArg))
            {
                stringifySession.InitReplacer(Js::RecyclableObject::FromVar(replacerArg));
            }
        }

        BEGIN_TEMP_ALLOCATOR(tempAlloc, scriptContext, _u("JSON"))
        {
            stringifySession.CompleteInit(space, tempAlloc);

            Js::DynamicObject* wrapper = scriptContext->GetLibrary()->CreateObject();
            JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(wrapper));
            Js::PropertyId propertyId = scriptContext->GetEmptyStringPropertyId();
            Js::JavascriptOperators::InitProperty(wrapper, propertyId, value);
            result = stringifySession.Str(scriptContext->GetLibrary()->GetEmptyString(), propertyId, wrapper, value);
        }
        END_TEMP_ALLOCATOR(tempAlloc, scriptContext);

        RELEASE_TEMP_GUEST_ALLOCATOR(nameTableAlloc, scriptContext);
        return result;
    }

    // -------- StringifySession implementation ------------//

    class JSONSpace
    {
    public:
        static char16  Buffer[JSONspaceSize];
        JSONSpace() { wmemset(Buffer, _u(' '), JSONspaceSize); }
    };
    char16 JSONSpace::Buffer[JSONspaceSize];
    static JSONSpace jsonSpace;

    void StringifySession::CompleteInit(Js::Var space, ArenaAllocator* tempAlloc)
    {
        charcount_t len = 0;
        switch (Js::JavascriptOperators::GetTypeId(space))
        {
        case Js::TypeIds_Integer:
            {
                len = max(0, min(JSONspaceSize, static_cast<int>(Js::TaggedInt::ToInt32(space))));
                if (len)
                {
                    gap = Js::JavascriptString::NewCopyBuffer(jsonSpace.Buffer, len, scriptContext);
                }
                break;
            }
        case Js::TypeIds_Number:
        case Js::TypeIds_NumberObject:
        case Js::TypeIds_Int64Number:
        case Js::TypeIds_UInt64Number:
            {
                len = max(0, static_cast<int>(min(static_cast<double>(JSONspaceSize), Js::JavascriptConversion::ToInteger(space, scriptContext))));
                if (len)
                {
                    gap = Js::JavascriptString::NewCopyBuffer(jsonSpace.Buffer, len, scriptContext);
                }
                break;
            }
        case Js::TypeIds_String:
            {
                len = min(static_cast<charcount_t>(JSONspaceSize), Js::JavascriptString::FromVar(space)->GetLength());
                if(len)
                {
                    gap = Js::JavascriptString::NewCopyBuffer(Js::JavascriptString::FromVar(space)->GetString(), len, scriptContext);
                }
                break;
            }
        case Js::TypeIds_StringObject:
            {
                Js::Var spaceString = Js::JavascriptConversion::ToString(space, scriptContext);
                if(Js::JavascriptString::Is(spaceString))
                {
                    len = min(static_cast<charcount_t>(JSONspaceSize), Js::JavascriptString::FromVar(spaceString)->GetLength());
                    if(len)
                    {
                        gap = Js::JavascriptString::NewCopyBuffer(Js::JavascriptString::FromVar(spaceString)->GetString(), len, scriptContext);
                    }
                }
                break;
            }
        }

        objectStack = Anew(tempAlloc, JSONStack, tempAlloc, scriptContext);
    }

    Js::Var StringifySession::Str(uint32 index, Js::RecyclableObject * holder)
    {
        Js::Var value = nullptr;
        Js::RecyclableObject *undefined = scriptContext->GetLibrary()->GetUndefined();

        if (Js::JavascriptArray::Is(holder->GetTypeId()) && !Js::JavascriptArray::FromAnyArray(holder)->IsCrossSiteObject())
        {
            value = Js::JavascriptArray::FromAnyArray(holder)->DirectGetItem(index);
            if (Js::JavascriptOperators::IsUndefinedObject(value, undefined))
            {
                return value;
            }
        }
        else
        {
            if (!Js::JavascriptOperators::GetItem(holder, index, &value, scriptContext))
            {
                return undefined;
            }
            if (Js::JavascriptOperators::IsUndefinedObject(value, undefined))
            {
                return value;
            }
        }

        Js::JavascriptString *key = scriptContext->GetIntegerString(index);
        return StrHelper(key, value, holder);
    }

    Js::Var StringifySession::Str(Js::JavascriptString* key, Js::PropertyId keyId, Js::Var holder, Js::Var value)
    {
        // We should look only into object's own properties here. When an object is serialized, only the own properties are considered,
        // the prototype chain is not considered. However, the property names can be selected via an array replacer. In this case
        // ES5 spec doesn't say the property has to own property or even to be enumerable. So, properties from the prototype, or non enum properties,
        // can end up being serialized. Well, that is the ES5 spec word.
        //if(!Js::RecyclableObject::FromVar(holder)->GetType()->GetProperty(holder, keyId, &value))

        if (value == nullptr)
        {
            Js::PropertyString* propertyString = Js::PropertyString::TryFromVar(key);
            if (propertyString)
            {
                PropertyValueInfo info;
                PropertyValueInfo::SetCacheInfo(&info, propertyString, propertyString->GetLdElemInlineCache(), false);
                CacheOperators::TryGetProperty<true, false, true, false, true, false, false, true, false>(holder, false, Js::RecyclableObject::FromVar(holder), keyId, &value, scriptContext, nullptr, &info);
            }

            if (value == nullptr)
            {
                if (!Js::JavascriptOperators::GetProperty(Js::RecyclableObject::FromVar(holder), keyId, &value, scriptContext))
                {
                    return scriptContext->GetLibrary()->GetUndefined();
                }
            }
        }

        return StrHelper(key, value, holder);
    }

    inline bool Get_ToJSON(ScriptContext* scriptContext, Js::JavascriptString* key, Js::Var* value, Js::TypeId typeId)
    {
        Js::Var toJSON = nullptr;
        Js::RecyclableObject* object = Js::RecyclableObject::FromVar(*value);
        while (typeId != Js::TypeIds_Null)
        {
            PropertyQueryFlags result = object->GetPropertyQuery(object, Js::PropertyIds::toJSON, &toJSON, nullptr, scriptContext);

            if (result != PropertyQueryFlags::Property_NotFound && Js::JavascriptConversion::IsCallable(toJSON))
            {
                Js::Var values[2];
                Js::Arguments args(2, values);
                args.Values[0] = *value;
                args.Values[1] = key;

                Js::RecyclableObject* func = Js::RecyclableObject::FromVar(toJSON);
                *value = Js::JavascriptFunction::CallFunction<true>(func, func->GetEntryPoint(), args);
                return true;
            }

            object = object->GetPrototype();
            typeId = Js::JavascriptOperators::GetTypeId(object);
        }
        return false;
    }

    Js::Var StringifySession::StrHelper(Js::JavascriptString* key, Js::Var value, Js::Var holder)
    {
        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);
        AssertMsg(Js::RecyclableObject::Is(holder), "The holder argument in a JSON::Str function must be an object");

        Js::Var undefined = scriptContext->GetLibrary()->GetUndefined();
        Js::TypeId id = Js::JavascriptOperators::GetTypeId(value);

        //check and apply 'toJSON' filter
        if (Js::JavascriptOperators::IsJsNativeObject(value) || (Js::JavascriptOperators::IsObject(value)))
        {
            if (Get_ToJSON(scriptContext, key, &value, id))
            {
                id = Js::JavascriptOperators::GetTypeId(value);
            }
        }

        //check and apply the user defined replacer filter
        if (ReplacerFunction == replacerType)
        {
            Js::Var values[3];
            Js::Arguments args(0, values);

            args.Info.Count = 3;
            args.Values[0] = holder;
            args.Values[1] = key;
            args.Values[2] = value;

            Js::RecyclableObject* func = replacer.ReplacerFunction;
            value = Js::JavascriptFunction::CallFunction<true>(func, func->GetEntryPoint(), args);
            id = Js::JavascriptOperators::GetTypeId(value);
        }

        if (Js::TypeIds_NumberObject == id)
        {
            value = Js::JavascriptNumber::ToVarNoCheck(Js::JavascriptConversion::ToNumber(value, scriptContext),scriptContext);
            id = Js::JavascriptOperators::GetTypeId(value);
        }
        else if (Js::TypeIds_StringObject == id)
        {
            value = Js::JavascriptConversion::ToString(value, scriptContext);
            id = Js::JavascriptOperators::GetTypeId(value);
        }
        else if (Js::TypeIds_BooleanObject == id)
        {
            value = Js::JavascriptBooleanObject::FromVar(value)->GetValue() ?
                scriptContext->GetLibrary()->GetTrue()
            :
                scriptContext->GetLibrary()->GetFalse();
            id = Js::JavascriptOperators::GetTypeId(value);
        }

        switch (id)
        {
        case Js::TypeIds_Undefined:
        case Js::TypeIds_Symbol:
            return undefined;

        case Js::TypeIds_Null:
            return scriptContext->GetLibrary()->GetNullDisplayString();

        case Js::TypeIds_Integer:
            return scriptContext->GetIntegerString(value);

        case Js::TypeIds_Boolean:
            return (Js::JavascriptBoolean::FromVar(value)->GetValue()) ? scriptContext->GetLibrary()->GetTrueDisplayString() : scriptContext->GetLibrary()->GetFalseDisplayString();

        case Js::TypeIds_Int64Number:
            if (Js::NumberUtilities::IsFinite(static_cast<double>(Js::JavascriptInt64Number::FromVar(value)->GetValue())))
            {
                return Js::JavascriptConversion::ToString(value, scriptContext);
            }
            else
            {
                return scriptContext->GetLibrary()->GetNullDisplayString();
            }

        case Js::TypeIds_UInt64Number:
            if (Js::NumberUtilities::IsFinite(static_cast<double>(Js::JavascriptUInt64Number::FromVar(value)->GetValue())))
            {
                return Js::JavascriptConversion::ToString(value, scriptContext);
            }
            else
            {
                return scriptContext->GetLibrary()->GetNullDisplayString();
            }

        case Js::TypeIds_Number:
            if (Js::NumberUtilities::IsFinite(Js::JavascriptNumber::GetValue(value)))
            {
                return Js::JavascriptConversion::ToString(value, scriptContext);
            }
            else
            {
                return scriptContext->GetLibrary()->GetNullDisplayString();
            }

        case Js::TypeIds_String:
            return Quote(Js::JavascriptString::FromVar(value));

        default:
            Js::Var ret = undefined;
            if (Js::JavascriptOperators::IsJsNativeObject(value))
            {
                if (!Js::JavascriptConversion::IsCallable(value))
                {
                    if (objectStack->Has(value))
                    {
                        Js::JavascriptError::ThrowTypeError(scriptContext, JSERR_JSONSerializeCircular);
                    }
                    objectStack->Push(value);

                    if (Js::JavascriptOperators::IsArray(value))
                    {
                        ret = StringifyArray(Js::RecyclableObject::FromVar(value));
                    }
                    else
                    {
                        ret = StringifyObject(value);
                    }
                    objectStack->Pop();
                }
            }
            else if (Js::JavascriptOperators::IsObject(value)) //every object which is not a native object gets stringified here
            {
                if (objectStack->Has(value, false))
                {
                    Js::JavascriptError::ThrowTypeError(scriptContext, JSERR_JSONSerializeCircular);
                }
                objectStack->Push(value, false);
                ret = StringifyObject(value);
                objectStack->Pop(false);
            }
            return ret;
        }
    }

    Js::Var StringifySession::StringifyObject(Js::Var value)
    {
        Js::JavascriptString* propertyName;
        Js::PropertyId id;
        Js::PropertyRecord const * propRecord;

        bool isFirstMember = true;
        bool isEmpty = true;

        uint stepBackIndent = this->indent++;
        Js::JavascriptString* memberSeparator = NULL;       // comma  or comma+linefeed+indent
        Js::JavascriptString* indentString = NULL;          // gap*indent
        Js::RecyclableObject* object = Js::RecyclableObject::FromVar(value);
        Js::JavascriptString* result = NULL;

        if(ReplacerArray == this->replacerType)
        {
            result = Js::ConcatStringBuilder::New(this->scriptContext, this->replacer.propertyList.length); // Reserve initial slots for properties.

            for (uint k = 0; k < this->replacer.propertyList.length;  k++)
            {
                propertyName = replacer.propertyList.propertyNames[k].propName;
                id = replacer.propertyList.propertyNames[k].propRecord->GetPropertyId();

                StringifyMemberObject(propertyName, id, value, (Js::ConcatStringBuilder*)result, indentString, memberSeparator, isFirstMember,  isEmpty);
            }
        }
        else
        {
            if (JavascriptProxy::Is(object))
            {
                JavascriptProxy* proxyObject = JavascriptProxy::FromVar(object);
                JavascriptArray* proxyResult = proxyObject->PropertyKeysTrap(JavascriptProxy::KeysTrapKind::GetOwnPropertyNamesKind, this->scriptContext);

                // filter enumerable keys
                uint32 resultLength = proxyResult->GetLength();
                result = Js::ConcatStringBuilder::New(this->scriptContext, resultLength);    // Reserve initial slots for properties.
                Var element;
                for (uint32 i = 0; i < resultLength; i++)
                {
                    element = proxyResult->DirectGetItem(i);

                    Assert(JavascriptString::Is(element));
                    propertyName = JavascriptString::FromVar(element);

                    PropertyDescriptor propertyDescriptor;
                    JavascriptConversion::ToPropertyKey(propertyName, scriptContext, &propRecord, nullptr);
                    id = propRecord->GetPropertyId();
                    if (JavascriptOperators::GetOwnPropertyDescriptor(RecyclableObject::FromVar(proxyObject), id, scriptContext, &propertyDescriptor))
                    {
                        if (propertyDescriptor.IsEnumerable())
                        {
                            StringifyMemberObject(propertyName, id, value, (Js::ConcatStringBuilder*)result, indentString, memberSeparator, isFirstMember, isEmpty);
                        }
                    }
                }
            }
            else
            {
                Js::JavascriptStaticEnumerator enumerator;
                if (object->GetEnumerator(&enumerator, EnumeratorFlags::SnapShotSemantics | EnumeratorFlags::EphemeralReference, scriptContext))
                {
                    bool isDynamicObject = Js::DynamicObject::Is(object);
                    Js::DynamicObject * dynamicObject = nullptr;
                    bool hasObjectArray = false;
                    if (isDynamicObject)
                    {
                        dynamicObject = Js::DynamicObject::FromVar(object);
                        hasObjectArray = dynamicObject->HasObjectArray();
                    }

                    bool repFunctionIsReplacerType = ReplacerFunction == replacerType;
                    bool requiresStoringNames = repFunctionIsReplacerType && (!isDynamicObject || hasObjectArray);
                    if (!requiresStoringNames)
                    {
                        uint32 propertyCount;

                        // if object has an objectArray, (aka indexed properties)
                        // we need to loop them first since they won't show up on the second loop below
                        // besides, we might be talking to a non dynamicObject
                        // in that case, we will also take the enumerator path
                        if (!isDynamicObject || hasObjectArray)
                        {
                            // get the precise count because resizing ConcatString will be expensive!
                            propertyCount = this->GetPropertyCount(object, &enumerator);
                            result = Js::ConcatStringBuilder::New(this->scriptContext, propertyCount);

                            enumerator.Reset();
                            while (propertyCount > 0 && (propertyName = enumerator.MoveAndGetNext(id)) != NULL)
                            {
                                // if unsuccessful get propertyId from the string
                                scriptContext->GetOrAddPropertyRecord(propertyName, &propRecord);
                                if (isDynamicObject && !propRecord->IsNumeric())
                                {
                                    break;
                                }

                                id = propRecord->GetPropertyId();
                                StringifyMemberObject(propertyName, id, value,
                                    (Js::ConcatStringBuilder*)result, indentString,
                                    memberSeparator, isFirstMember, isEmpty);
                                propertyCount--;
                            }
                        }

                        if (isDynamicObject)
                        {
                            Js::DynamicTypeHandler * typeHandler = dynamicObject->GetTypeHandler();
                            propertyCount = dynamicObject->GetPropertyCount(); // logic below assumes the whole unfiltered count

                            if (result == NULL)
                            {
                                result = Js::ConcatStringBuilder::New(this->scriptContext, propertyCount);
                            }

                            // loop over the named properties
                            for (uint32 i = 0; i < propertyCount; i++)
                            {
                                id = typeHandler->GetPropertyId(scriptContext, (Js::PropertyId)i);
                                if (id == Js::Constants::NoProperty)
                                {
                                    continue;
                                }

                                if (!dynamicObject->IsEnumerable(id))
                                {
                                    continue;
                                }

                                Js::PropertyString * propertyString = scriptContext->GetPropertyString(id);
                                propRecord = propertyString->GetPropertyRecord();
                                propertyName = (Js::JavascriptString*) propertyString;

                                if (!propRecord->IsSymbol())
                                {
                                    PropertyIndex index = typeHandler->GetPropertyIndex(propRecord);
                                    Js::Var property = index != Constants::NoSlot ?
                                        dynamicObject->GetSlot(index)
                                    :
                                        nullptr; // slow case. isCaller?

                                    StringifyMemberObject(propertyName, id, value,
                                      (Js::ConcatStringBuilder*)result, indentString,
                                      memberSeparator, isFirstMember, isEmpty, property);
                                }
                            }
                        }
                    }
                    else // case: ES5 && ReplacerFunction == replacerType. && object->HasObjectArray
                    {
                        // TODO: Optimize this? Store only the ObjectArray and loop over the params
                        Js::Var* nameTable = nullptr;
                        // ES5 requires that the new properties introduced by the replacer to not be stringified
                        // Get the actual count first.
                        uint precisePropertyCount = (uint) this->GetPropertyCount(object, &enumerator);

                        result = Js::ConcatStringBuilder::New(this->scriptContext, precisePropertyCount);    // Reserve initial slots for properties.

                        // pick the property names before walking the object
                        DECLARE_TEMP_GUEST_ALLOCATOR(nameTableAlloc);
                        if (precisePropertyCount > 0)
                        {
                            // allocate and fill a table with the property names
                            if (precisePropertyCount < MAX_JSON_STRINGIFY_NAMES_ON_STACK)
                            {
                                PROBE_STACK(scriptContext, (sizeof(Js::Var) * precisePropertyCount));
                                nameTable = (Js::Var*)_alloca(sizeof(Js::Var) * precisePropertyCount);
                            } else
                            {
                                ACQUIRE_TEMP_GUEST_ALLOCATOR(nameTableAlloc, scriptContext, _u("JSON"));
                                nameTable = AnewArray(nameTableAlloc, Js::Var, precisePropertyCount);
                            }
                            enumerator.Reset();
                            uint index = 0;
                            while ((propertyName = enumerator.MoveAndGetNext(id)) != NULL && index < precisePropertyCount)
                            {
                                nameTable[index++] = propertyName;
                            }

                            // walk the property name list
                            // Note that we're only walking up to index, not precisePropertyCount, as we only know that we've filled the array up to index
                            for (uint k = 0; k < index; k++)
                            {
                                propertyName = Js::JavascriptString::FromVar(nameTable[k]);
                                scriptContext->GetOrAddPropertyRecord(propertyName, &propRecord);
                                id = propRecord->GetPropertyId();
                                StringifyMemberObject(propertyName, id, value,
                                  (Js::ConcatStringBuilder*)result, indentString,
                                  memberSeparator, isFirstMember, isEmpty);
                            }
                        }
                        RELEASE_TEMP_GUEST_ALLOCATOR(nameTableAlloc, scriptContext);
                    }
                }
            }
        }
        Assert(isEmpty || result);

        if(isEmpty)
        {
            result = scriptContext->GetLibrary()->GetEmptyObjectString();
        }
        else
        {
            if(this->gap)
            {
                JavascriptLibrary *library = scriptContext->GetLibrary();
                if(!indentString)
                {
                    indentString = GetIndentString(this->indent);
                }
                // Note: it's better to use strings with length = 1 as the are cached/new instances are not created every time.
                Js::ConcatStringN<7>* retVal = Js::ConcatStringN<7>::New(this->scriptContext);
                retVal->SetItem(0, library->GetOpenBracketString());
                retVal->SetItem(1, library->GetNewLineString());
                retVal->SetItem(2, indentString);
                retVal->SetItem(3, result);
                retVal->SetItem(4, library->GetNewLineString());
                retVal->SetItem(5, GetIndentString(stepBackIndent));
                retVal->SetItem(6, library->GetCloseBracketString());
                result = retVal;
            }
            else
            {
                result = Js::ConcatStringWrapping<_u('{'), _u('}')>::New(result);
            }
        }

        this->indent = stepBackIndent;
        return result;
    }

    Js::JavascriptString* StringifySession::GetArrayElementString(uint32 index, Js::RecyclableObject * arrayValue)
    {
        Js::RecyclableObject *undefined = scriptContext->GetLibrary()->GetUndefined();

        Js::Var arrayElement = Str(index, arrayValue);
        if (Js::JavascriptOperators::IsUndefinedObject(arrayElement, undefined))
        {
            return scriptContext->GetLibrary()->GetNullDisplayString();
        }
        return Js::JavascriptString::FromVar(arrayElement);
    }

    Js::Var StringifySession::StringifyArray(Js::RecyclableObject * value)
    {
        uint stepBackIndent = this->indent++;
        Js::JavascriptString* memberSeparator = NULL;       // comma  or comma+linefeed+indent
        Js::JavascriptString* indentString = NULL;          // gap*indent

        uint32 length;
        if (Js::JavascriptArray::Is(value->GetTypeId()))
        {
            length = Js::JavascriptArray::FromAnyArray(value)->GetLength();
        }
        else
        {
            int64 len = Js::JavascriptConversion::ToLength(Js::JavascriptOperators::OP_GetLength(value, scriptContext), scriptContext);
            if (MaxCharCount <= len)
            {
                // If the length goes more than MaxCharCount we will eventually fail (as OOM) in ConcatStringBuilder - so failing early.
                JavascriptError::ThrowRangeError(scriptContext, JSERR_OutOfBoundString);
            }
            length = (uint32)len;
        }

        Js::JavascriptString* result;
        if (length == 0)
        {
            result = scriptContext->GetLibrary()->GetEmptyArrayString();
        }
        else
        {
            if (length == 1)
            {
                result = GetArrayElementString(0, value);
            }
            else
            {
                Assert(length > 1);
                if (!indentString)
                {
                    indentString = GetIndentString(this->indent);
                    memberSeparator = GetMemberSeparator(indentString);
                }
                bool isFirstMember = true;

                // Total node count: number of array elements (N = length) + indents [including member separators] (N = length - 1).
                result = Js::ConcatStringBuilder::New(this->scriptContext, length * 2 - 1);
                for (uint32 k = 0; k < length; k++)
                {
                    if (!isFirstMember)
                    {
                        ((Js::ConcatStringBuilder*)result)->Append(memberSeparator);
                    }
                    Js::JavascriptString* arrayElementString = GetArrayElementString(k, value);
                    ((Js::ConcatStringBuilder*)result)->Append(arrayElementString);
                    isFirstMember = false;
                }
            }

            if (this->gap)
            {
                JavascriptLibrary *library = scriptContext->GetLibrary();
                if (!indentString)
                {
                    indentString = GetIndentString(this->indent);
                }
                Js::ConcatStringN<7>* retVal = Js::ConcatStringN<7>::New(this->scriptContext);
                retVal->SetItem(0, library->GetOpenSBracketString());
                retVal->SetItem(1, library->GetNewLineString());
                retVal->SetItem(2, indentString);
                retVal->SetItem(3, result);
                retVal->SetItem(4, library->GetNewLineString());
                retVal->SetItem(5, GetIndentString(stepBackIndent));
                retVal->SetItem(6, library->GetCloseSBracketString());
                result = retVal;
            }
            else
            {
                result = Js::ConcatStringWrapping<_u('['), _u(']')>::New(result);
            }
        }

        this->indent = stepBackIndent;
        return result;
    }

    Js::JavascriptString* StringifySession::GetPropertySeparator()
    {
        if(!propertySeparator)
        {
            if(this->gap)
            {
                propertySeparator = scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u(": "));
            }
            else
            {
                propertySeparator = scriptContext->GetLibrary()->GetColonString();
            }
        }
        return propertySeparator;
    }

    Js::JavascriptString* StringifySession::GetIndentString(uint count)
    {
        // Note: this potentially can be improved by using a special ConcatString which has gap and count fields.
        //       Although this does not seem to be a critical path (using gap should not be often).
        Js::JavascriptString* res = scriptContext->GetLibrary()->GetEmptyString();
        if(this->gap)
        {
            for (uint i = 0 ; i < count; i++)
            {
                res = Js::JavascriptString::Concat(res, this->gap);
            }
        }
        return res;
    }

    Js::JavascriptString* StringifySession::GetMemberSeparator(Js::JavascriptString* indentString)
    {
        if(this->gap)
        {
            return Js::JavascriptString::Concat(scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u(",\n")), indentString);
        }
        else
        {
            return scriptContext->GetLibrary()->GetCommaDisplayString();
        }
    }

    void StringifySession::StringifyMemberObject(Js::JavascriptString* propertyName,
      Js::PropertyId id, Js::Var value, Js::ConcatStringBuilder* result,
      Js::JavascriptString* &indentString, Js::JavascriptString* &memberSeparator,
      bool &isFirstMember, bool &isEmpty, Js::Var propertyValue)
    {
        Js::Var propertyObjectString = Str(propertyName, id, value, propertyValue);
        if(!Js::JavascriptOperators::IsUndefinedObject(propertyObjectString))
        {
            int slotIndex = 0;
            Js::ConcatStringN<4>* tempResult = Js::ConcatStringN<4>::New(this->scriptContext);   // We may use 3 or 4 slots.
            if(!isFirstMember)
            {
                if(!indentString)
                {
                    indentString = GetIndentString(this->indent);
                    memberSeparator = GetMemberSeparator(indentString);
                }
                tempResult->SetItem(slotIndex++, memberSeparator);
            }
            tempResult->SetItem(slotIndex++, Quote(propertyName));
            tempResult->SetItem(slotIndex++, this->GetPropertySeparator());
            tempResult->SetItem(slotIndex++, Js::JavascriptString::FromVar(propertyObjectString));

            result->Append(tempResult);
            isFirstMember = false;
            isEmpty = false;
        }
    }

    // Returns precise property count for given object and enumerator, does not count properties that are undefined.
    inline uint32 StringifySession::GetPropertyCount(Js::RecyclableObject* object, Js::JavascriptStaticEnumerator* enumerator)
    {
        uint32 count = 0;
        Js::JavascriptString * propertyName;
        Js::PropertyId id;
        enumerator->Reset();
        while ((propertyName = enumerator->MoveAndGetNext(id)) != NULL)
        {
            ++count;
        }
        return count;
    }

    // Returns property count (including array items) for given object and enumerator.
    // When object has objectArray, we do slow path return actual/precise count, in this case *pPrecise will receive true.
    // Otherwise we optimize for speed and try to guess the count, in this case *pPrecise will receive false.
    // Parameters:
    // - object: the object to get the number of properties for.
    // - enumerator: the enumerator to enumerate the object.
    // - [out] pIsPrecise: receives a boolean indicating whether the value returned is precise or just guessed.
    inline uint32 StringifySession::GetPropertyCount(Js::RecyclableObject* object, Js::JavascriptStaticEnumerator* enumerator, bool* pIsPrecise)
    {
        Assert(pIsPrecise);
        *pIsPrecise = false;

        uint32 count = object->GetPropertyCount();
        if (Js::DynamicObject::Is(object) && Js::DynamicObject::FromVar(object)->HasObjectArray())
        {
            // Can't use array->GetLength() as this can be sparse array for which we stringify only real/set properties.
            // Do one walk through the elements.
            // This would account for prototype property as well.
            count = this->GetPropertyCount(object, enumerator);
            *pIsPrecise = true;
        }
        if (!*pIsPrecise && count > sizeof(Js::JavascriptString*) * 8)
        {
            // For large # of elements just one more for potential prototype wouldn't matter.
            ++count;
        }

        return count;
    }

    inline Js::JavascriptString* StringifySession::Quote(Js::JavascriptString* value)
    {
        // By default, optimize for scenario when we don't need to change the inside of the string. That's majority of cases.
        return Js::JSONString::Escape<Js::EscapingOperation_NotEscape>(value);
    }
} // namespace JSON
