// Copyright Epic Games, Inc. All Rights Reserved.

#include "ViewModel/MVVMViewModelBlueprintCompiler.h"
#include "HAL/IConsoleManager.h"
#include "ViewModel/MVVMViewModelBlueprint.h"
#include "KismetCompiledFunctionContext.h"
#include "ViewModel/MVVMViewModelBlueprintGeneratedClass.h"
#include "MVVMViewModelBase.h"

#include "FieldNotification/CustomizationHelper.h"
#include "FieldNotification/FieldNotificationHelpers.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "MVVMFunctionGraphHelper.h"

#define LOCTEXT_NAMESPACE "ViewModelBlueprintCompiler"

namespace UE::MVVM
{

static TAutoConsoleVariable<bool> CVarAutogenerateSetter(
	TEXT("MVVM.Viewmodel.GenerateSetter"),
	true,
	TEXT("If true, the setter function will be always be autogenerated"),
	ECVF_Default);

static TAutoConsoleVariable<bool> CVarAutogenerateOnRep(
	TEXT("MVVM.Viewmodel.GenerateOnRep"),
	true,
	TEXT("If true, the OnRep function will be always be autogenerated"),
	ECVF_Default);

//////////////////////////////////////////////////////////////////////////
// FViewModelBlueprintCompiler 

bool FViewModelBlueprintCompiler::CanCompile(const UBlueprint* Blueprint)
{
	return Cast<UMVVMViewModelBlueprint>(Blueprint) != nullptr;
}


void FViewModelBlueprintCompiler::Compile(UBlueprint* Blueprint, const FKismetCompilerOptions& CompileOptions, FCompilerResultsLog& Results)
{
	if (UMVVMViewModelBlueprint* WidgetBlueprint = CastChecked<UMVVMViewModelBlueprint>(Blueprint))
	{
		FViewModelBlueprintCompilerContext Compiler(WidgetBlueprint, Results, CompileOptions);
		Compiler.Compile();
		check(Compiler.NewClass);
	}
}


bool FViewModelBlueprintCompiler::GetBlueprintTypesForClass(UClass* ParentClass, UClass*& OutBlueprintClass, UClass*& OutBlueprintGeneratedClass) const
{
	if (ParentClass == UMVVMViewModelBase::StaticClass() || ParentClass->IsChildOf(UMVVMViewModelBase::StaticClass()))
	{
		OutBlueprintClass = UMVVMViewModelBlueprint::StaticClass();
		OutBlueprintGeneratedClass = UMVVMViewModelBlueprintGeneratedClass::StaticClass();
		return true;
	}

	return false;
}


//////////////////////////////////////////////////////////////////////////
// FViewModelBlueprintCompilerContext


UMVVMViewModelBlueprint* FViewModelBlueprintCompilerContext::GetViewModelBlueprint() const
{
	return Cast<UMVVMViewModelBlueprint>(Blueprint);
}


FProperty* FViewModelBlueprintCompilerContext::FindPropertyByNameOnNewClass(FName PropertyName) const
{
	for (FField* CurrentField = NewClass->ChildProperties; CurrentField != nullptr; CurrentField = CurrentField->Next)
	{
		if (CurrentField->GetFName() == PropertyName)
		{
			return CastField<FProperty>(CurrentField);
		}
	}
	return nullptr;
}


void FViewModelBlueprintCompilerContext::SaveSubObjectsFromCleanAndSanitizeClass(FSubobjectCollection& SubObjectsToSave, UBlueprintGeneratedClass* ClassToClean)
{
	Super::SaveSubObjectsFromCleanAndSanitizeClass(SubObjectsToSave, ClassToClean);

	// Make sure our typed pointer is set
	check(ClassToClean == NewClass);
	NewViewModelBlueprintClass = CastChecked<UMVVMViewModelBlueprintGeneratedClass>((UObject*)NewClass);
}


void FViewModelBlueprintCompilerContext::CleanAndSanitizeClass(UBlueprintGeneratedClass* ClassToClean, UObject*& InOutOldCDO)
{
	Super::CleanAndSanitizeClass(ClassToClean, InOutOldCDO);

	NewViewModelBlueprintClass->FieldNotifyNames.Empty();
}


void FViewModelBlueprintCompilerContext::CreateFunctionList()
{
	for (const FGeneratedFunction& Function : GeneratedFunctions)
	{
		if (Function.Property == nullptr && Function.SkelProperty == nullptr)
		{
			MessageLog.Error(*FString::Printf(TEXT("Internal error. The property '%s' is invalid. Setter and net rep was not generated."), *Function.PropertyName.ToString()));
			continue;
		}

		if (Function.SetterFunction)
		{
			if (!FunctionGraphHelper::GenerateViewModelFieldNotifySetter(*this, Function.SetterFunction, Function.SkelProperty, TEXT("NewValue")))
			{
				MessageLog.Error(*FString::Printf(TEXT("The setter function for '%s' could not be generated."), *Function.PropertyName.ToString()));
				continue;
			}
		}
		if (Function.NetRepFunction)
		{
			if (!FunctionGraphHelper::GenerateViewModelFielNotifyBroadcast(*this, Function.NetRepFunction, Function.SkelProperty))
			{
				MessageLog.Error(*FString::Printf(TEXT("The net rep function for '%s' could not be generated."), *Function.PropertyName.ToString()));
				continue;
			}
		}
	}

	Super::CreateFunctionList();
}


void FViewModelBlueprintCompilerContext::CreateClassVariablesFromBlueprint()
{
	Super::CreateClassVariablesFromBlueprint();

	if (CompileOptions.CompileType == EKismetCompileType::SkeletonOnly)
	{
		auto RenameObjectToTransientPackage = [](UObject* ObjectToRename)
		{
			const ERenameFlags RenFlags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
			ObjectToRename->Rename(nullptr, GetTransientPackage(), RenFlags);
			ObjectToRename->SetFlags(RF_Transient);
			ObjectToRename->ClearFlags(RF_Public | RF_Standalone | RF_ArchetypeObject);
			FLinkerLoad::InvalidateExport(ObjectToRename);
		};

		for (UEdGraph* OldGraph : GetViewModelBlueprint()->TemporaryGraph)
		{
			if (OldGraph)
			{
				RenameObjectToTransientPackage(OldGraph);
			}
		}
		GetViewModelBlueprint()->TemporaryGraph.Reset();
	}

	const FName NAME_MetaDataBlueprintSetter = "BlueprintSetter";
	const FName NAME_Category = "Category";
	for (TFieldIterator<FProperty> PropertyIt(NewClass, EFieldIterationFlags::None); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		FName PropertyName = Property->GetFName();

		bool bGenerateSetterFunction = CVarAutogenerateSetter.GetValueOnGameThread() || Property->HasMetaData(UE::FieldNotification::FCustomizationHelper::MetaData_FieldNotify);
		bool bGenerateReplicatedFunction = CVarAutogenerateOnRep.GetValueOnGameThread(); // Property->RepNotifyFunc.IsNone();
		if (bGenerateSetterFunction || bGenerateReplicatedFunction)
		{
			Property->SetPropertyFlags(CPF_DisableEditOnInstance);

			const FString SetterName = FString::Printf(TEXT("Set%s"), *Property->GetName());
			const FString NetRepName = FString::Printf(TEXT("__RepNotify_%s"), *Property->GetName());

#if WITH_EDITOR
			if (bGenerateSetterFunction)
			{
				Property->SetMetaData(FBlueprintMetadata::MD_PropertySetFunction, *SetterName);
			}
#endif

			if (CompileOptions.CompileType == EKismetCompileType::SkeletonOnly)
			{
				FGeneratedFunction GeneratedFunction;
				GeneratedFunction.PropertyName = PropertyName;
				GeneratedFunction.SkelProperty = Property;

				if (GeneratedFunctions.ContainsByPredicate([PropertyName](const FGeneratedFunction& Other)
					{
						return Other.PropertyName == PropertyName;
					}))
				{
					MessageLog.Error(*FString::Printf(TEXT("Internal error. The functions are already generated for properpty."), *PropertyName.ToString()));
					continue;
				}

				if (bGenerateSetterFunction)
				{
					GeneratedFunction.SetterFunction = FunctionGraphHelper::CreateIntermediateFunctionGraph(
						*this
						, *SetterName
						, (FUNC_BlueprintCallable | FUNC_Public)
						, Property->GetMetaData(NAME_Category)
						, false);
					GetViewModelBlueprint()->TemporaryGraph.Add(GeneratedFunction.SetterFunction);

					if (GeneratedFunction.SetterFunction == nullptr)
					{
						MessageLog.Error(*FString::Printf(TEXT("The setter name '%s' already exists and could not be autogenerated."), *SetterName));
						continue;
					}

					if (GeneratedFunction.SetterFunction->GetFName() != *SetterName)
					{
						MessageLog.Error(*FString::Printf(TEXT("The setter name '%s' was generated with a different name."), *SetterName));
						continue;
					}

					FunctionGraphHelper::AddFunctionArgument(GeneratedFunction.SetterFunction, Property, TEXT("NewValue"));
				}

				if (bGenerateReplicatedFunction)
				{
					GeneratedFunction.NetRepFunction = FunctionGraphHelper::CreateIntermediateFunctionGraph(
						*this
						, *NetRepName
						, (FUNC_Private)
						, Property->GetMetaData(NAME_Category)
						, false);
					GetViewModelBlueprint()->TemporaryGraph.Add(GeneratedFunction.NetRepFunction);

					if (GeneratedFunction.NetRepFunction == nullptr)
					{
						MessageLog.Error(*FString::Printf(TEXT("The netrep function name '%s' already exists and could not be autogenerated."), *NetRepName));
						continue;
					}

					if (GeneratedFunction.NetRepFunction->GetFName() != *NetRepName)
					{
						MessageLog.Error(*FString::Printf(TEXT("The netrep function name '%s' was generated with a different name."), *NetRepName));
						continue;
					}

					Property->SetPropertyFlags(CPF_Net);
					Property->RepNotifyFunc = GeneratedFunction.NetRepFunction->GetFName();

					FunctionGraphHelper::AddFunctionArgument(GeneratedFunction.SetterFunction, Property, TEXT("NewValue"));
				}

				GeneratedFunctions.Add(GeneratedFunction);
			}

			NewViewModelBlueprintClass->FieldNotifyNames.Emplace(Property->GetFName());
		}
	}
}


void FViewModelBlueprintCompilerContext::SpawnNewClass(const FString& NewClassName)
{
	NewViewModelBlueprintClass = FindObject<UMVVMViewModelBlueprintGeneratedClass>(Blueprint->GetOutermost(), *NewClassName);

	if (NewViewModelBlueprintClass == nullptr)
	{
		NewViewModelBlueprintClass = NewObject<UMVVMViewModelBlueprintGeneratedClass>(Blueprint->GetOutermost(), FName(*NewClassName), RF_Public | RF_Transactional);
	}
	else
	{
		// Already existed, but wasn't linked in the Blueprint yet due to load ordering issues
		FBlueprintCompileReinstancer::Create(NewViewModelBlueprintClass);
	}
	NewClass = NewViewModelBlueprintClass;
}


void FViewModelBlueprintCompilerContext::OnNewClassSet(UBlueprintGeneratedClass* ClassToUse)
{
	NewViewModelBlueprintClass = CastChecked<UMVVMViewModelBlueprintGeneratedClass>(ClassToUse);
}


void FViewModelBlueprintCompilerContext::PostcompileFunction(FKismetFunctionContext& Context)
{
	Super::PostcompileFunction(Context);

	if (Context.Function && Context.Function->HasMetaData(UE::FieldNotification::FCustomizationHelper::MetaData_FieldNotify))
	{
		if (!UE::FieldNotification::Helpers::IsValidAsField(Context.Function))
		{
			MessageLog.Error(*LOCTEXT("FieldNotify_IsEventGraph", "Function @@ cannot be a FieldNotify. A function needs to be const, returns a single properties, has no inputs, not be an event or a net function.").ToString(), Context.EntryPoint);
		}
		NewViewModelBlueprintClass->FieldNotifyNames.Emplace(Context.Function->GetFName());
	}
}


void FViewModelBlueprintCompilerContext::FinishCompilingClass(UClass* Class)
{
	Super::FinishCompilingClass(Class);

	const bool bIsSkeletonOnly = CompileOptions.CompileType == EKismetCompileType::SkeletonOnly;
	if (!bIsSkeletonOnly)
	{
		if (UMVVMViewModelBlueprintGeneratedClass* BPGClass = CastChecked<UMVVMViewModelBlueprintGeneratedClass>(Class))
		{
			if (UMVVMViewModelBase* ViewModelBase = Cast<UMVVMViewModelBase>(BPGClass->GetDefaultObject()))
			{
				BPGClass->InitializeFieldNotification(ViewModelBase);
			}
		}
	}
}

} //namespace

#undef LOCTEXT_NAMESPACE