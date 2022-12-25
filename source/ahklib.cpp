/*
ahklib.cpp

Original code by Steve Gray.

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, without restriction.
*/

#include "stdafx.h"
#include "globaldata.h"
#include "DispObject.h"
#include "script_func_impl.h"

#include "autogenerated/ahklib_h.h"
#include "autogenerated/ahklib_i.c"


#define AHKAPI(_rettype_) extern "C" __declspec(dllexport) _rettype_ __stdcall


void TokenToVariant(ExprTokenType &aToken, VARIANT &aVar, BOOL aVarIsArg);

void EarlyAppInit();
ResultType InitForExecution();
int MainExecuteScript(bool aMsgSleep = true);


AHKAPI(int) Main(int argc, LPTSTR argv[])
{
	__argc = argc;
	__targv = argv;
	// _tWinMain() doesn't use lpCmdLine or nShowCmd.
	return _tWinMain(g_hInstance, NULL, nullptr, SW_SHOWNORMAL);
}


class DECLSPEC_NOVTABLE EnumVARIANT : public IEnumVARIANT
{
	ULONG mRefCount;

protected:
	ULONG mIndex;

public:
	EnumVARIANT() : mRefCount(0), mIndex(0) {}

	virtual ~EnumVARIANT() {}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppvObject)
	{
		if (riid == IID_IUnknown || riid == IID_IEnumVARIANT)
			*ppvObject = static_cast<IEnumVARIANT*>(this);
		else
			return E_NOTIMPL;
		AddRef();
		return S_OK;
	}

	STDMETHODIMP_(ULONG) AddRef()
	{
		return ++mRefCount;
	}

	STDMETHODIMP_(ULONG) Release()
	{
		if (mRefCount)
			return --mRefCount;
		delete this;
		return 0;
	}

	STDMETHODIMP Next(ULONG celt, /*out*/ VARIANT *rgVar, /*out*/ ULONG *pCeltFetched) = 0;
	
	STDMETHODIMP Skip(ULONG celt) { return E_NOTIMPL; }
	STDMETHODIMP Reset() { return E_NOTIMPL; }
	STDMETHODIMP Clone(/*out*/ IEnumVARIANT **ppEnum) { return E_NOTIMPL; }
};


class DescribeFunc : public DispObject<IDescribeFunc>
{
	Func *mFunc;

public:
	DescribeFunc(ITypeInfo *typeInfo) : DispObject<IDescribeFunc>(typeInfo) {}

	static HRESULT Create(Func *func, IDispatch **ppDisp)
	{
		auto hr = CreateDispatchInstance<DescribeFunc>(ppDisp);
		if (FAILED(hr))
			return hr;
		static_cast<DescribeFunc*>(*ppDisp)->mFunc = func;
		return S_OK;
	}
	
	STDMETHODIMP_(BSTR) get_Name() { return SysAllocString(mFunc->mName); }
	STDMETHODIMP_(BSTR) get_File() { return SysAllocString(Line::sSourceFile[mFunc->mFileIndex]); }
	STDMETHODIMP_(UINT) get_Line() { return mFunc->mLineNumber; }
	STDMETHODIMP_(UINT) get_EndLine();
	STDMETHODIMP_(BOOL) get_IsBuiltIn() { return mFunc->mIsBuiltIn; }
	STDMETHODIMP_(BOOL) get_IsVariadic() { return mFunc->mIsVariadic; }
	STDMETHODIMP_(int) get_MinParams() { return mFunc->mMinParams; }
	STDMETHODIMP_(int) get_MaxParams() { return mFunc->mParamCount; }
	STDMETHODIMP_(HRESULT) get_Vars(IDispatch** ppDisp);
	STDMETHODIMP_(HRESULT) get_Params(IDispatch** Value);
	STDMETHODIMP_(HRESULT) get_Globals(IDispatch** Value);
	STDMETHODIMP_(BSTR) get_DefaultVarType();
};


class EnumFuncs : public EnumVARIANT
{
public:
	EnumFuncs() : EnumVARIANT() {}

	STDMETHODIMP Next(ULONG celt, /*out*/ VARIANT *rgVar, /*out*/ ULONG *pCeltFetched)
	{
		ULONG i;
		for (i = 0; i < celt && mIndex < (ULONG)g_script.mFuncCount; ++i, ++mIndex)
		{
			rgVar->vt = VT_DISPATCH;
			auto hr = DescribeFunc::Create(g_script.mFunc[mIndex], &rgVar->pdispVal);
			if (FAILED(hr))
			{
				if (pCeltFetched)
					*pCeltFetched = i;
				return hr;
			}
		}
		if (pCeltFetched)
			*pCeltFetched = i;
		return i < celt ? S_FALSE : S_OK;
	}
};


class FuncCollection : public DispObject<IDispCollection>
{
public:
	FuncCollection(ITypeInfo *typeInfo) : DispObject<IDispCollection>(typeInfo) {}

	STDMETHODIMP get_Item(VARIANT* index, VARIANT *value)
	{
		if (index->vt != VT_BSTR)
			return DISP_E_TYPEMISMATCH;
		auto func = g_script.FindFunc(index->bstrVal);
		if (!func)
			return DISP_E_BADINDEX;
		value->vt = VT_DISPATCH;
		return DescribeFunc::Create(func, &value->pdispVal);
	}

	STDMETHODIMP get_Count(int *pCount)
	{
		*pCount = g_script.mFuncCount;
		return S_OK;
	}

	STDMETHODIMP _NewEnum(IEnumVARIANT **ppEnum)
	{
		*ppEnum = new EnumFuncs();
		return S_OK;
	}
};


class DescribeParam : public DispObject<IDescribeParam>
{
	FuncParam *mParam;
	bool mIsRest;

public:
	DescribeParam(ITypeInfo *typeInfo) : DispObject<IDescribeParam>(typeInfo) {}

	static HRESULT Create(FuncParam *param, bool isRest, IDispatch **ppDisp)
	{
		auto hr = CreateDispatchInstance<DescribeParam>(ppDisp);
		if (FAILED(hr))
			return hr;
		static_cast<DescribeParam*>(*ppDisp)->mParam = param;
		static_cast<DescribeParam*>(*ppDisp)->mIsRest = isRest;
		return S_OK;
	}

	STDMETHODIMP_(BSTR) get_Name() { return SysAllocString(mParam->var->mName); }
	STDMETHODIMP_(BOOL) get_IsByRef() { return mParam->is_byref; }
	STDMETHODIMP_(BOOL) get_IsOptional() { return mParam->default_type != PARAM_DEFAULT_NONE; }
	STDMETHODIMP_(BOOL) get_IsRest() { return mIsRest; }

	STDMETHODIMP get_Default(VARIANT* value)
	{
		switch (mParam->default_type)
		{
		case PARAM_DEFAULT_INT:
			value->vt = ((int)mParam->default_int64 == mParam->default_int64) ? VT_I4 : VT_I8;
			value->llVal = mParam->default_int64;
			break;
		case PARAM_DEFAULT_FLOAT:
			value->vt = VT_R8;
			value->dblVal = mParam->default_double;
			break;
		case PARAM_DEFAULT_STR:
			value->vt = VT_BSTR;
			value->bstrVal = SysAllocString(mParam->default_str);
			break;
		default:
			value->vt = VT_EMPTY;
		}
		return S_OK;
	}
};


class EnumParams : public EnumVARIANT
{
	Func *mFunc;

public:
	EnumParams(Func *func) : mFunc(func) {}

	STDMETHODIMP Next(ULONG celt, /*out*/ VARIANT *rgVar, /*out*/ ULONG *pCeltFetched)
	{
		ULONG i;
		for (i = 0; i < celt && mIndex < (ULONG)mFunc->mParamCount + mFunc->mIsVariadic; ++i, ++mIndex)
		{
			rgVar->vt = VT_DISPATCH;
			auto hr = DescribeParam::Create(mFunc->mParam + mIndex, mIndex == mFunc->mParamCount, &rgVar->pdispVal);
			if (FAILED(hr))
			{
				if (pCeltFetched)
					*pCeltFetched = i;
				return hr;
			}
		}
		if (pCeltFetched)
			*pCeltFetched = i;
		return i < celt ? S_FALSE : S_OK;
	}
};


class ParamCollection : public DispObject<IDispCollection>
{
	Func *mFunc;

public:
	ParamCollection(ITypeInfo *typeInfo) : DispObject<IDispCollection>(typeInfo) {}

	static HRESULT Create(Func *func, IDispatch **ppDisp)
	{
		auto hr = CreateDispatchInstance<ParamCollection>(ppDisp);
		if (FAILED(hr))
			return hr;
		static_cast<ParamCollection*>(*ppDisp)->mFunc = func;
		return S_OK;
	}

	STDMETHODIMP get_Item(VARIANT* index, VARIANT *value)
	{
		if (index->vt != VT_I4)
			return DISP_E_TYPEMISMATCH;
		if (index->lVal < 1 || index->lVal > mFunc->mParamCount + mFunc->mIsVariadic)
			return DISP_E_BADINDEX;
		value->vt = VT_DISPATCH;
		return DescribeParam::Create(mFunc->mParam + index->lVal - 1, index->lVal > mFunc->mParamCount, &value->pdispVal);
	}

	STDMETHODIMP get_Count(int *pCount)
	{
		*pCount = mFunc->mParamCount + mFunc->mIsVariadic;
		return S_OK;
	}

	STDMETHODIMP _NewEnum(IEnumVARIANT **ppEnum)
	{
		*ppEnum = new EnumParams(mFunc);
		return S_OK;
	}
};


class DescribeVar : public DispObject<IDescribeVar>
{
	Var *mVar;

public:
	DescribeVar(ITypeInfo *typeInfo) : DispObject<IDescribeVar>(typeInfo) {}

	static HRESULT Create(Var *var, IDispatch **ppDisp)
	{
		auto hr = CreateDispatchInstance<DescribeVar>(ppDisp);
		if (FAILED(hr))
			return hr;
		static_cast<DescribeVar*>(*ppDisp)->mVar = var;
		return S_OK;
	}

	STDMETHODIMP_(BSTR) get_Name() { return SysAllocString(mVar->mName); }
	STDMETHODIMP_(BOOL) get_IsReadOnly() { return VAR_IS_READONLY(*mVar); }
	STDMETHODIMP_(BOOL) get_IsVirtual() { return mVar->Type() >= VAR_CLIPBOARD; }
	STDMETHODIMP_(BOOL) get_IsDeclared() { return mVar->IsDeclared(); }
	STDMETHODIMP_(BOOL) get_IsSuperGlobal() { return mVar->IsSuperGlobal(); }
};


class VarCollection;
class EnumVars : public EnumVARIANT
{
	VarCollection *mVars;

public:
	EnumVars(VarCollection *vars);
	~EnumVars();

	STDMETHODIMP Next(ULONG celt, /*out*/ VARIANT *rgVar, /*out*/ ULONG *pCeltFetched);
};


class VarCollection : public DispObject<IDispCollection>
{
	Var **mVar, **mLazyVar;
	int mCount, mLazyCount;

	friend class EnumVars;

public:
	VarCollection(ITypeInfo *typeInfo) : DispObject<IDispCollection>(typeInfo) {}

	static HRESULT Create(Var **var, int count, Var **lazyVar, int lazyCount, IDispatch **ppDisp)
	{
		auto hr = CreateDispatchInstance<VarCollection>(ppDisp);
		if (FAILED(hr))
			return hr;
		auto &inst = *static_cast<VarCollection*>(*ppDisp);
		inst.mVar = var;
		inst.mCount = count;
		inst.mLazyVar = lazyVar;
		inst.mLazyCount = lazyCount;
		return S_OK;
	}

	STDMETHODIMP get_Item(VARIANT* index, VARIANT *value)
	{
		if (index->vt != VT_BSTR)
			return DISP_E_TYPEMISMATCH;
		auto name = value->bstrVal;
		Var *var = nullptr;
		if (mVar != g_script.mVar)
		{
			for (int i = 0; i < mCount; ++i)
				if (!_tcsicmp(mVar[i]->mName, name))
				{
					var = mVar[i];
					break;
				}
			if (!var)
				for (int i = 0; i < mLazyCount; ++i)
				{
					if (!_tcsicmp(mLazyVar[i]->mName, name))
					{
						var = mLazyVar[i];
						break;
					}
				}
		}
		else
			var = g_script.FindVar(name, 0, nullptr, FINDVAR_GLOBAL);
		if (!var)
			return DISP_E_BADINDEX;
		value->vt = VT_DISPATCH;
		return DescribeVar::Create(var, &value->pdispVal);
	}

	STDMETHODIMP get_Count(int *pCount)
	{
		*pCount = mCount + mLazyCount;
		return S_OK;
	}

	STDMETHODIMP _NewEnum(IEnumVARIANT **ppEnum)
	{
		*ppEnum = new EnumVars(this);
		return S_OK;
	}
};


EnumVars::EnumVars(VarCollection *vars) : EnumVARIANT(), mVars(vars) { mVars->AddRef(); }
EnumVars::~EnumVars() { mVars->Release(); }


STDMETHODIMP EnumVars::Next(ULONG celt, /*out*/ VARIANT *rgVar, /*out*/ ULONG *pCeltFetched)
{
	ULONG i, total_count = mVars->mCount + mVars->mLazyCount;
	for (i = 0; i < celt && mIndex < total_count; ++i, ++mIndex)
	{
		Var *var = mIndex < (ULONG)mVars->mCount ? mVars->mVar[mIndex] : mVars->mLazyVar[mIndex - (ULONG)mVars->mCount];
		rgVar[i].vt = VT_DISPATCH;
		auto hr = DescribeVar::Create(var, &rgVar[i].pdispVal);
		if (FAILED(hr))
		{
			if (pCeltFetched)
				*pCeltFetched = i;
			return hr;
		}
	}
	if (pCeltFetched)
		*pCeltFetched = i;
	return i < celt ? S_FALSE : S_OK;
}


UINT DescribeFunc::get_EndLine()
{
	if (!mFunc->mIsBuiltIn)
	{
		for (Line *line = mFunc->mJumpToLine; line; line = line->mNextLine)
			if (line->mActionType == ACT_BLOCK_END && line->mAttribute)
				return line->mLineNumber;
	}
	return 0;
}


HRESULT DescribeFunc::get_Vars(IDispatch** ppDisp)
{
	return VarCollection::Create(mFunc->mVar, mFunc->mVarCount, mFunc->mLazyVar, mFunc->mLazyVarCount, ppDisp);
}


HRESULT DescribeFunc::get_Params(IDispatch** ppDisp)
{
	if (mFunc->mIsBuiltIn)
		return E_NOTIMPL;
	return ParamCollection::Create(mFunc, ppDisp);
}


HRESULT DescribeFunc::get_Globals(IDispatch** ppDisp)
{
	return VarCollection::Create(mFunc->mGlobalVar, mFunc->mGlobalVarCount, nullptr, 0, ppDisp);
}


BSTR DescribeFunc::get_DefaultVarType()
{
	LPCTSTR mode = nullptr;
	switch (mFunc->mDefaultVarType)
	{
	case VAR_DECLARE_LOCAL | VAR_FORCE_LOCAL: mode = _T("local"); break;
	case VAR_DECLARE_STATIC | VAR_FORCE_LOCAL: mode = _T("local static"); break;
	case VAR_DECLARE_STATIC: mode = _T("static"); break;
	case VAR_DECLARE_GLOBAL: mode = _T("global"); break;
	}
	return mode ? SysAllocString(mode) : nullptr;
}


class DescribeLabel : public DispObject<IDescribeLabel>
{
	Label *mLabel;

public:
	DescribeLabel(ITypeInfo *typeInfo) : DispObject<IDescribeLabel>(typeInfo) {}

	static HRESULT Create(Label *label, IDispatch **ppDisp)
	{
		auto hr = CreateDispatchInstance<DescribeLabel>(ppDisp);
		if (FAILED(hr))
			return hr;
		static_cast<DescribeLabel*>(*ppDisp)->mLabel = label;
		return S_OK;
	}
	
	STDMETHODIMP_(BSTR) get_Name() { return SysAllocString(mLabel->mName); }
	STDMETHODIMP_(BSTR) get_File() { return SysAllocString(Line::sSourceFile[mLabel->mFileIndex]); }
	STDMETHODIMP_(UINT) get_Line() { return mLabel->mLineNumber; }
};


class EnumLabels : public EnumVARIANT
{
	Label *mCurrLabel;

public:
	EnumLabels() : EnumVARIANT(), mCurrLabel(g_script.mFirstLabel) {}

	STDMETHODIMP Next(ULONG celt, /*out*/ VARIANT *rgVar, /*out*/ ULONG *pCeltFetched)
	{
		ULONG i;
		for (i = 0; i < celt && mCurrLabel; ++i, mCurrLabel = mCurrLabel->mNextLabel)
		{
			rgVar[i].vt = VT_DISPATCH;
			auto hr = DescribeLabel::Create(mCurrLabel, &rgVar[i].pdispVal);
			if (FAILED(hr))
			{
				if (pCeltFetched)
					*pCeltFetched = i;
				return hr;
			}
		}
		if (pCeltFetched)
			*pCeltFetched = i;
		return i < celt ? S_FALSE : S_OK;
	}
};


class LabelCollection : public DispObject<IDispCollection>
{
public:
	LabelCollection(ITypeInfo *typeInfo) : DispObject<IDispCollection>(typeInfo) {}

	STDMETHODIMP get_Item(VARIANT* index, VARIANT *value)
	{
		if (index->vt != VT_BSTR)
			return DISP_E_TYPEMISMATCH;
		Label *label = g_script.FindLabel(index->bstrVal);
		if (!label)
			return DISP_E_BADINDEX;
		value->vt = VT_DISPATCH;
		return DescribeLabel::Create(label, &value->pdispVal);
	}

	STDMETHODIMP get_Count(int *pCount)
	{
		*pCount = g_script.mLabelCount;
		return S_OK;
	}

	STDMETHODIMP _NewEnum(IEnumVARIANT **ppEnum)
	{
		*ppEnum = new EnumLabels();
		return S_OK;
	}
};


class AutoHotkeyScript : public ObjectBase
{
	ResultType STDMETHODCALLTYPE Invoke(ExprTokenType &aResultToken, ExprTokenType &aThisToken, int aFlags, ExprTokenType *aParam[], int aParamCount)
	{
		LPTSTR name = ParamIndexToString(0, _f_retval_buf);
		if (IS_INVOKE_CALL) // Script.Function()
		{
			auto func = g_script.FindFunc(name);
			if (!func)
				return INVOKE_NOT_HANDLED;
			return func->Invoke(aResultToken, ExprTokenType(func), IT_CALL | IF_FUNCOBJ, aParam + 1, aParamCount - 1);
		}
		auto var = g_script.FindVar(name, 0, nullptr, FINDVAR_GLOBAL);
		if (!var)
			return INVOKE_NOT_HANDLED;
		if (aParamCount > (IS_INVOKE_SET ? 2 : 1)) // Script.Var[Index]
		{
			auto obj = var->HasObject() ? var->Object() : nullptr;
			if (!obj)
				return INVOKE_NOT_HANDLED;
			return obj->Invoke(aResultToken, ExprTokenType(obj), aFlags, aParam + 1, aParamCount - 1);
		}
		if (IS_INVOKE_SET) // Script.Var := Value
			return var->Assign(*aParam[1]);
		// Script.Var
		if (var->Type() == VAR_NORMAL)
		{
			var->ToToken(aResultToken);
			return OK;
		}
		// Built-in/virtual variable
		if (!TokenSetResult(aResultToken, nullptr, var->Get()))
			return FAIL;
		aResultToken.marker_length = var->Get(aResultToken.marker);
		return OK;
	}

	IObject_Type_Impl("Script")
};


class AutoHotkeyLib : public DispObject<IAutoHotkeyLib>
{
	AutoHotkeyScript *mScript;

public:
	AutoHotkeyLib(ITypeInfo *typeInfo) : DispObject<IAutoHotkeyLib>(typeInfo), mScript(nullptr) {}


	STDMETHODIMP Main(BSTR cmdLine, int *exitCode)
	{
		if (Line::sSourceFileCount)
			return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
		int argc;
		auto argv = CommandLineToArgvW(cmdLine, &argc);
		*exitCode = ::Main(argc, argv);
		LocalFree(argv);
		return S_OK;
	}


	STDMETHODIMP LoadFile(LPTSTR aScriptPath)
	{
		if (Line::sSourceFileCount)
			return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
		EarlyAppInit();
		if (!g_script.Init(aScriptPath))
			return E_FAIL;
		if (g_script.LoadFromFile(aScriptPath) == LOADING_FAILED
			|| !InitForExecution())
			return E_FAIL;
		return S_OK;
	}


	STDMETHODIMP Execute(int *aExitCode)
	{
		if (!g_script.mIsReadyToExecute)
			return E_FAIL;
		*aExitCode = MainExecuteScript(false);
		return S_OK;
	}


	STDMETHODIMP get_Script(IDispatch **ppScript)
	{
		if (!mScript)
			mScript = new AutoHotkeyScript;
		mScript->AddRef();
		*ppScript = mScript;
		return S_OK;
	}


	STDMETHODIMP get_Funcs(IDispatch **ppFuncs)
	{
		return CreateDispatchInstance<FuncCollection>(ppFuncs);
	}

	STDMETHODIMP get_Vars(IDispatch **ppVars)
	{
		return VarCollection::Create(g_script.mVar, g_script.mVarCount, g_script.mLazyVar, g_script.mLazyVarCount, ppVars);
	}

	STDMETHODIMP get_Labels(IDispatch **ppLabels)
	{
		return CreateDispatchInstance<LabelCollection>(ppLabels);
	}


	STDMETHODIMP get_Files(SAFEARRAY **psa)
	{
		*psa = SafeArrayCreateVector(VT_BSTR, 1, Line::sSourceFileCount);
		if (!*psa)
			return E_OUTOFMEMORY;
		for (LONG i = 1; i <= Line::sSourceFileCount; ++i)
			SafeArrayPutElement(*psa, &i, SysAllocString(Line::sSourceFile[i-1]));
		return S_OK;
	}
};


AHKAPI(HRESULT) Host(IDispatch **ppLib)
{
	return CreateDispatchInstance<AutoHotkeyLib>(ppLib);
}


BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch( fdwReason ) 
	{ 
	case DLL_PROCESS_ATTACH:
		g_hInstance = hinstDLL;
		break;
	case DLL_PROCESS_DETACH:
		if (lpvReserved != nullptr)
			break;
		// Perform any necessary cleanup.
		break;
	}
	return TRUE;
}


HRESULT LoadMyTypeLib(ITypeLib **ppTypeLib)
{
	TCHAR lib_path[MAX_PATH + 1];
	DWORD len = GetModuleFileName(g_hInstance, lib_path, _countof(lib_path));
	if (!len || len == _countof(lib_path))
		return E_FAIL;
	return LoadTypeLib(lib_path, ppTypeLib);
}


HRESULT LoadMyTypeInfo(REFIID riid, ITypeInfo **ppTypeInfo)
{
	ITypeLib* pTypeLib;
	if (FAILED(LoadMyTypeLib(&pTypeLib)))
		return E_FAIL;
	HRESULT hr = pTypeLib->GetTypeInfoOfGuid(riid, ppTypeInfo);
	pTypeLib->Release();
	return hr;
}
