//===--- DerivedConformanceCodable.cpp - Derived Codable ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements explicit derivation of the Encodable and Decodable
// protocols for a struct or class.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "DerivedConformances.h"

using namespace swift;

/// Returns whether the type represented by the given ClassDecl inherits from a
/// type which conforms to the given protocol.
static bool superclassConformsTo(ClassDecl *target, KnownProtocolKind kpk) {
  if (!target) {
    return false;
  }

  auto superclass = target->getSuperclassDecl();
  if (!superclass)
    return false;

  return !superclass
              ->getModuleContext()
              ->lookupConformance(target->getSuperclass(),
                                  target->getASTContext().getProtocol(kpk))
              .isInvalid();
}

/// Retrieve the variable name for the purposes of encoding/decoding.
static Identifier getVarNameForCoding(VarDecl *var) {
  if (auto originalVar = var->getOriginalWrappedProperty())
    return originalVar->getName();

  return var->getName();
}

/// Combine two identifiers separated by an '_'
static Identifier combineIdentifiers(const ASTContext &C, Identifier first,
                                     Identifier second) {
  std::string enumIdentifierName = first.str().str() + "_" + second.str().str();
  return C.getIdentifier(StringRef(enumIdentifierName));
}

/// Validates the given CodingKeys enum decl by ensuring its cases are a 1-to-1
/// match with the stored vars of the given type.
///
/// \param codingKeysDecl The \c CodingKeys enum decl to validate.
static bool validateCodingKeysEnum(const DerivedConformance &derived,
                                   EnumDecl *codingKeysDecl) {
  auto conformanceDC = derived.getConformanceContext();

  // Look through all var decls in the given type.
  // * Filter out lazy/computed vars.
  // * Filter out ones which are present in the given decl (by name).
  //
  // If any of the entries in the CodingKeys decl are not present in the type
  // by name, then this decl doesn't match.
  // If there are any vars left in the type which don't have a default value
  // (for Decodable), then this decl doesn't match.

  // Here we'll hold on to properties by name -- when we've validated a property
  // against its CodingKey entry, it will get removed.
  llvm::SmallMapVector<Identifier, VarDecl *, 8> properties;
  for (auto *varDecl : derived.Nominal->getStoredProperties()) {
    if (!varDecl->isUserAccessible())
      continue;

    properties[getVarNameForCoding(varDecl)] = varDecl;
  }

  bool propertiesAreValid = true;
  for (auto elt : codingKeysDecl->getAllElements()) {
    auto it = properties.find(elt->getBaseIdentifier());
    if (it == properties.end()) {
      elt->diagnose(diag::codable_extraneous_codingkey_case_here,
                    elt->getBaseIdentifier());
      // TODO: Investigate typo-correction here; perhaps the case name was
      //       misspelled and we can provide a fix-it.
      propertiesAreValid = false;
      continue;
    }

    // We have a property to map to. Ensure it's {En,De}codable.
    auto target =
        conformanceDC->mapTypeIntoContext(it->second->getValueInterfaceType());
    if (TypeChecker::conformsToProtocol(target, derived.Protocol, conformanceDC)
            .isInvalid()) {
      TypeLoc typeLoc = {
          it->second->getTypeReprOrParentPatternTypeRepr(),
          it->second->getType(),
      };
      it->second->diagnose(diag::codable_non_conforming_property_here,
                           derived.getProtocolType(), typeLoc);
      propertiesAreValid = false;
    } else {
      // The property was valid. Remove it from the list.
      properties.erase(it);
    }
  }

  if (!propertiesAreValid)
    return false;

  // If there are any remaining properties which the CodingKeys did not cover,
  // we can skip them on encode. On decode, though, we can only skip them if
  // they have a default value.
  if (derived.Protocol->isSpecificProtocol(KnownProtocolKind::Decodable)) {
    for (auto &entry : properties) {
      const auto *pbd = entry.second->getParentPatternBinding();
      if (pbd && pbd->isDefaultInitializable()) {
        continue;
      }

      if (entry.second->isParentInitialized()) {
        continue;
      }

      // The var was not default initializable, and did not have an explicit
      // initial value.
      propertiesAreValid = false;
      entry.second->diagnose(diag::codable_non_decoded_property_here,
                             derived.getProtocolType(), entry.first);
    }
  }

  return propertiesAreValid;
}

/// Validates the given CodingKeys enum decl by ensuring its cases are a 1-to-1
/// match with the stored vars of the given EnumElementDecl.
///
/// \param elementDecl The \c EnumElementDecl to validate against.
/// \param codingKeysDecl The \c CodingKeys enum decl to validate.
static bool validateCaseCodingKeysEnum(const DerivedConformance &derived,
                                       EnumElementDecl *elementDecl,
                                       EnumDecl *codingKeysDecl) {
  auto conformanceDC = derived.getConformanceContext();

  // Look through all var decls in the given type.
  // * Filter out lazy/computed vars.
  // * Filter out ones which are present in the given decl (by name).
  //
  // If any of the entries in the CodingKeys decl are not present in the type
  // by name, then this decl doesn't match.
  // If there are any vars left in the type which don't have a default value
  // (for Decodable), then this decl doesn't match.

  // Here we'll hold on to properties by name -- when we've validated a property
  // against its CodingKey entry, it will get removed.
  llvm::SmallMapVector<Identifier, ParamDecl *, 8> properties;
  if (elementDecl->hasAssociatedValues()) {
    for (auto *varDecl : elementDecl->getParameterList()->getArray()) {
      if (!varDecl->isUserAccessible())
        continue;

      properties[getVarNameForCoding(varDecl)] = varDecl;
    }
  }

  bool propertiesAreValid = true;
  for (auto elt : codingKeysDecl->getAllElements()) {
    auto it = properties.find(elt->getBaseIdentifier());
    if (it == properties.end()) {
      elt->diagnose(diag::codable_extraneous_codingkey_case_here,
                    elt->getBaseIdentifier());
      // TODO: Investigate typo-correction here; perhaps the case name was
      //       misspelled and we can provide a fix-it.
      propertiesAreValid = false;
      continue;
    }

    // We have a property to map to. Ensure it's {En,De}codable.
    auto target =
        conformanceDC->mapTypeIntoContext(it->second->getValueInterfaceType());
    if (TypeChecker::conformsToProtocol(target, derived.Protocol, conformanceDC)
            .isInvalid()) {
      TypeLoc typeLoc = {
          it->second->getTypeReprOrParentPatternTypeRepr(),
          it->second->getType(),
      };
      it->second->diagnose(diag::codable_non_conforming_property_here,
                           derived.getProtocolType(), typeLoc);
      propertiesAreValid = false;
    } else {
      // The property was valid. Remove it from the list.
      properties.erase(it);
    }
  }

  if (!propertiesAreValid)
    return false;

  // If there are any remaining properties which the CodingKeys did not cover,
  // we can skip them on encode. On decode, though, we can only skip them if
  // they have a default value.
  if (derived.Protocol->isSpecificProtocol(KnownProtocolKind::Decodable)) {
    for (auto &entry : properties) {
      if (entry.second->hasDefaultExpr()) {
        continue;
      }

      // The var was not default initializable, and did not have an explicit
      // initial value.
      propertiesAreValid = false;
      entry.second->diagnose(diag::codable_non_decoded_property_here,
                             derived.getProtocolType(), entry.first);
    }
  }

  return propertiesAreValid;
}

/// A type which has information about the validity of an encountered
/// \c CodingKeys type.
enum class CodingKeysClassification {
  /// A \c CodingKeys declaration was found, but it is invalid.
  Invalid,
  /// No \c CodingKeys declaration was found, so it must be synthesized.
  NeedsSynthesizedCodingKeys,
  /// A valid \c CodingKeys declaration was found.
  Valid,
};

/// Returns whether the given ValueDecl is an enum conforming to the CodingKey
/// protocol.
///
/// \returns An EnumDecl* to the passed in ValueDecl if it is valid, nullptr
/// otherwise.
static EnumDecl *
validateCodingKeysProtocolConformance(const DerivedConformance &derived,
                                      ValueDecl *decl) {
  auto &C = derived.Context;

  auto *codingKeysTypeDecl = dyn_cast<TypeDecl>(decl);
  if (!codingKeysTypeDecl) {
    decl->diagnose(diag::codable_codingkeys_type_is_not_an_enum_here,
                   derived.getProtocolType());
    return nullptr;
  }

  // CodingKeys may be a typealias. If so, follow the alias to its canonical
  // type.
  auto codingKeysType = codingKeysTypeDecl->getDeclaredInterfaceType();
  if (isa<TypeAliasDecl>(codingKeysTypeDecl))
    codingKeysTypeDecl = codingKeysType->getAnyNominal();

  // Ensure that the type we found conforms to the CodingKey protocol.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  if (!TypeChecker::conformsToProtocol(codingKeysType, codingKeyProto,
                                       derived.getConformanceContext())) {
    // If CodingKeys is a typealias which doesn't point to a valid nominal type,
    // codingKeysTypeDecl will be nullptr here. In that case, we need to warn on
    // the location of the usage, since there isn't an underlying type to
    // diagnose on.
    SourceLoc loc = codingKeysTypeDecl ? codingKeysTypeDecl->getLoc()
                                       : cast<TypeDecl>(decl)->getLoc();

    C.Diags.diagnose(loc, diag::codable_codingkeys_type_does_not_conform_here,
                     derived.getProtocolType());

    return nullptr;
  }

  // CodingKeys must be an enum for synthesized conformance.
  auto *codingKeysEnum = dyn_cast<EnumDecl>(codingKeysTypeDecl);
  if (!codingKeysEnum) {
    codingKeysTypeDecl->diagnose(
        diag::codable_codingkeys_type_is_not_an_enum_here,
        derived.getProtocolType());
    return nullptr;
  }

  return codingKeysEnum;
}

/// Returns whether the given type has a valid nested \c CodingKeys enum.
///
/// If the type has an invalid \c CodingKeys entity, produces diagnostics to
/// complain about the error. In this case, the error result will be true -- in
/// the case where we don't have a valid CodingKeys enum and have produced
/// diagnostics here, we don't want to then attempt to synthesize a CodingKeys
/// enum.
///
/// \returns A \c CodingKeysValidity value representing the result of the check.
static CodingKeysClassification
classifyCodingKeys(const DerivedConformance &derived) {
  auto &C = derived.Context;
  auto codingKeysDecls =
      derived.Nominal->lookupDirect(DeclName(C.Id_CodingKeys));

  bool needsSynthesis = codingKeysDecls.empty();

  // Only ill-formed code would produce multiple results for this lookup.
  // This would get diagnosed later anyway, so we're free to only look at the
  // first result here.
  EnumDecl *codingKeysEnum = nullptr;
  if (!needsSynthesis) {
    codingKeysEnum =
        validateCodingKeysProtocolConformance(derived, codingKeysDecls.front());
    if (!codingKeysEnum) {
      return CodingKeysClassification::Invalid;
    }
  }

  if (auto *enumDecl = dyn_cast<EnumDecl>(derived.Nominal)) {
    for (auto *elt : enumDecl->getAllElements()) {
      Identifier caseCodingKeyId =
          combineIdentifiers(C, C.Id_CodingKeys, elt->getBaseIdentifier());
      auto caseCodingKeysDecls =
          derived.Nominal->lookupDirect(DeclName(caseCodingKeyId));
      if (caseCodingKeysDecls.empty()) {
        needsSynthesis = true;
      } else {
        auto *caseCodingKeysEnum = validateCodingKeysProtocolConformance(
            derived, caseCodingKeysDecls.front());
        if (!caseCodingKeysEnum) {
          return CodingKeysClassification::Invalid;
        }

        if (!validateCaseCodingKeysEnum(derived, elt, caseCodingKeysEnum)) {
          return CodingKeysClassification::Invalid;
        }
      }
    }
    return needsSynthesis ? CodingKeysClassification::NeedsSynthesizedCodingKeys
                          : CodingKeysClassification::Valid;
  } else if (needsSynthesis) {
    return CodingKeysClassification::NeedsSynthesizedCodingKeys;
  } else {
    return validateCodingKeysEnum(derived, codingKeysEnum)
               ? CodingKeysClassification::Valid
               : CodingKeysClassification::Invalid;
  }
}

/// Fetches the \c CodingKeys enum nested in \c target, potentially reaching
/// through a typealias if the "CodingKeys" entity is a typealias.
///
/// This is only useful once a \c CodingKeys enum has been validated (via \c
/// hasValidCodingKeysEnum) or synthesized (via \c synthesizeCodingKeysEnum).
///
/// \param C The \c ASTContext to perform the lookup in.
///
/// \param target The target type to look in.
///
/// \return A retrieved canonical \c CodingKeys enum if \c target has a valid
/// one; \c nullptr otherwise.
static EnumDecl *lookupEvaluatedCodingKeysEnum(ASTContext &C,
                                               NominalTypeDecl *target) {
  auto codingKeyDecls = target->lookupDirect(DeclName(C.Id_CodingKeys));
  if (codingKeyDecls.empty())
    return nullptr;

  auto *codingKeysDecl = codingKeyDecls.front();
  if (auto *typealiasDecl = dyn_cast<TypeAliasDecl>(codingKeysDecl))
    codingKeysDecl = typealiasDecl->getDeclaredInterfaceType()->getAnyNominal();

  return dyn_cast<EnumDecl>(codingKeysDecl);
}

static EnumDecl *lookupEvaluatedCodingKeysEnum(ASTContext &C,
                                               NominalTypeDecl *target,
                                               Identifier identifier) {
  auto codingKeyDecls = target->lookupDirect(DeclName(identifier));
  if (codingKeyDecls.empty())
    return nullptr;

  auto *codingKeysDecl = codingKeyDecls.front();
  if (auto *typealiasDecl = dyn_cast<TypeAliasDecl>(codingKeysDecl))
    codingKeysDecl = typealiasDecl->getDeclaredInterfaceType()->getAnyNominal();

  return dyn_cast<EnumDecl>(codingKeysDecl);
}

static EnumElementDecl *lookupEnumCase(ASTContext &C, NominalTypeDecl *target,
                                       Identifier identifier) {
  auto elementDecls = target->lookupDirect(DeclName(identifier));
  if (elementDecls.empty())
    return nullptr;

  auto *elementDecl = elementDecls.front();

  return dyn_cast<EnumElementDecl>(elementDecl);
}

/// Synthesizes a new \c CodingKeys enum based on the {En,De}codable members of
/// the given type (\c nullptr if unable to synthesize).
///
/// If able to synthesize the enum, adds it directly to \c derived.Nominal.
static bool synthesizeCodingKeysEnum_enum(DerivedConformance &derived,
                                          EnumDecl *target) {
  auto &C = derived.Context;

  // We want to look through all the var declarations of this type to create
  // enum cases based on those var names.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  auto codingKeyType = codingKeyProto->getDeclaredInterfaceType();
  TypeLoc protoTypeLoc[1] = {TypeLoc::withoutLoc(codingKeyType)};
  ArrayRef<TypeLoc> inherited = C.AllocateCopy(protoTypeLoc);

  bool allConform = true;
  auto *conformanceDC = derived.getConformanceContext();

  auto add = [conformanceDC, &C, &derived](VarDecl *varDecl,
                                           EnumDecl *enumDecl) {
    if (!varDecl->isUserAccessible()) {
      return true;
    }

    auto target =
        conformanceDC->mapTypeIntoContext(varDecl->getValueInterfaceType());
    if (TypeChecker::conformsToProtocol(target, derived.Protocol, conformanceDC)
            .isInvalid()) {
      TypeLoc typeLoc = {
          varDecl->getTypeReprOrParentPatternTypeRepr(),
          varDecl->getType(),
      };
      varDecl->diagnose(diag::codable_non_conforming_property_here,
                        derived.getProtocolType(), typeLoc);

      return false;
    } else {
      // if the type conforms to {En,De}codable, add it to the enum.
      auto *elt =
          new (C) EnumElementDecl(SourceLoc(), getVarNameForCoding(varDecl),
                                  nullptr, SourceLoc(), nullptr, enumDecl);
      elt->setImplicit();
      enumDecl->addMember(elt);

      return true;
    }
  };

  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, target);

  // Only derive CodingKeys enum if it is not already defined
  if (!codingKeysEnum) {
    auto *enumDecl = new (C) EnumDecl(SourceLoc(), C.Id_CodingKeys, SourceLoc(),
                                      inherited, nullptr, target);
    enumDecl->setImplicit();
    enumDecl->setAccess(AccessLevel::Private);

    for (auto *elementDecl : target->getAllElements()) {
      auto *elt =
          new (C) EnumElementDecl(SourceLoc(), elementDecl->getBaseName(),
                                  nullptr, SourceLoc(), nullptr, enumDecl);
      elt->setImplicit();
      enumDecl->addMember(elt);
    }
    // Forcibly derive conformance to CodingKey.
    TypeChecker::checkConformancesInContext(enumDecl);

    // Add to the type.
    target->addMember(enumDecl);
    codingKeysEnum = enumDecl;
  }

  for (auto *elementDecl : target->getAllElements()) {
    auto enumIdentifier = combineIdentifiers(C, C.Id_CodingKeys,
                                             elementDecl->getBaseIdentifier());

    // Only derive if this case exist in the CodingKeys enum
    auto *codingKeyCase =
        lookupEnumCase(C, codingKeysEnum, elementDecl->getBaseIdentifier());
    if (!codingKeyCase)
      continue;

    // Only derive if it is not already defined
    if (!derived.Nominal->lookupDirect(DeclName(enumIdentifier)).empty())
      continue;

    // If there are any unnamed parameters, we can't generate CodingKeys for
    // this element and it will be encoded into an unkeyed container.
    if (elementDecl->hasAnyUnnamedParameters())
      continue;

    auto *nestedEnum = new (C) EnumDecl(
        SourceLoc(), enumIdentifier, SourceLoc(), inherited, nullptr, target);
    nestedEnum->setImplicit();
    nestedEnum->setAccess(AccessLevel::Private);

    auto *elementParams = elementDecl->getParameterList();
    if (elementParams) {
      for (auto *paramDecl : elementParams->getArray()) {
        allConform = allConform && add(paramDecl, nestedEnum);
      }
    }

    // Forcibly derive conformance to CodingKey.
    TypeChecker::checkConformancesInContext(nestedEnum);

    target->addMember(nestedEnum);
  }

  if (!allConform)
    return false;

  return true;
}

/// Synthesizes a new \c CodingKeys enum based on the {En,De}codable members of
/// the given type (\c nullptr if unable to synthesize).
///
/// If able to synthesize the enum, adds it directly to \c derived.Nominal.
static bool synthesizeCodingKeysEnum(DerivedConformance &derived) {
  auto &C = derived.Context;
  // Create CodingKeys in the parent type always, because both
  // Encodable and Decodable might want to use it, and they may have
  // different conditional bounds. CodingKeys is simple and can't
  // depend on those bounds.
  auto target = derived.Nominal;

  if (auto *targetEnum = dyn_cast<EnumDecl>(target))
    return synthesizeCodingKeysEnum_enum(derived, targetEnum);

  // We want to look through all the var declarations of this type to create
  // enum cases based on those var names.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  auto codingKeyType = codingKeyProto->getDeclaredInterfaceType();
  TypeLoc protoTypeLoc[1] = {TypeLoc::withoutLoc(codingKeyType)};
  ArrayRef<TypeLoc> inherited = C.AllocateCopy(protoTypeLoc);

  auto *enumDecl = new (C) EnumDecl(SourceLoc(), C.Id_CodingKeys, SourceLoc(),
                                    inherited, nullptr, target);
  enumDecl->setImplicit();
  enumDecl->setSynthesized();
  enumDecl->setAccess(AccessLevel::Private);

  // For classes which inherit from something Encodable or Decodable, we
  // provide case `super` as the first key (to be used in encoding super).
  auto *classDecl = dyn_cast<ClassDecl>(target);
  if (superclassConformsTo(classDecl, KnownProtocolKind::Encodable) ||
      superclassConformsTo(classDecl, KnownProtocolKind::Decodable)) {
    // TODO: Ensure the class doesn't already have or inherit a variable named
    // "`super`"; otherwise we will generate an invalid enum. In that case,
    // diagnose and bail.
    auto *super = new (C) EnumElementDecl(SourceLoc(), C.Id_super, nullptr,
                                          SourceLoc(), nullptr, enumDecl);
    super->setImplicit();
    enumDecl->addMember(super);
  }

  // Each of these vars needs a case in the enum. For each var decl, if the type
  // conforms to {En,De}codable, add it to the enum.
  bool allConform = true;
  auto *conformanceDC = derived.getConformanceContext();
  for (auto *varDecl : target->getStoredProperties()) {
    if (!varDecl->isUserAccessible()) {
      continue;
    }

    auto target =
        conformanceDC->mapTypeIntoContext(varDecl->getValueInterfaceType());
    if (TypeChecker::conformsToProtocol(target, derived.Protocol, conformanceDC)
            .isInvalid()) {
      TypeLoc typeLoc = {
          varDecl->getTypeReprOrParentPatternTypeRepr(),
          varDecl->getType(),
      };
      varDecl->diagnose(diag::codable_non_conforming_property_here,
                        derived.getProtocolType(), typeLoc);
      allConform = false;
    } else {
      auto *elt =
          new (C) EnumElementDecl(SourceLoc(), getVarNameForCoding(varDecl),
                                  nullptr, SourceLoc(), nullptr, enumDecl);
      elt->setImplicit();
      enumDecl->addMember(elt);
    }
  }

  if (!allConform)
    return false;

  // Forcibly derive conformance to CodingKey.
  TypeChecker::checkConformancesInContext(enumDecl);

  // Add to the type.
  target->addMember(enumDecl);
  return true;
}

/// Creates a new var decl representing
///
///   var/let container : containerBase<keyType>
///
/// \c containerBase is the name of the type to use as the base (either
/// \c KeyedEncodingContainer or \c KeyedDecodingContainer).
///
/// \param C The AST context to create the decl in.
///
/// \param DC The \c DeclContext to create the decl in.
///
/// \param keyedContainerDecl The generic type to bind the key type in.
///
/// \param keyType The key type to bind to the container type.
///
/// \param introducer Whether to declare the variable as immutable.
///
/// \param name Name of the resulting variable
static VarDecl *createKeyedContainer(ASTContext &C, DeclContext *DC,
                                     NominalTypeDecl *keyedContainerDecl,
                                     Type keyType,
                                     VarDecl::Introducer introducer,
                                     Identifier name) {
  // Bind Keyed*Container to Keyed*Container<KeyType>
  Type boundType[1] = {keyType};
  auto containerType = BoundGenericType::get(keyedContainerDecl, Type(),
                                             C.AllocateCopy(boundType));

  // let container : Keyed*Container<KeyType>
  auto *containerDecl =
      new (C) VarDecl(/*IsStatic=*/false, introducer, SourceLoc(), name, DC);
  containerDecl->setImplicit();
  containerDecl->setSynthesized();
  containerDecl->setInterfaceType(containerType);
  return containerDecl;
}

/// Creates a new var decl representing
///
///   var/let container : containerBase
///
/// \c containerBase is the name of the type to use as the base (either
/// \c UnkeyedEncodingContainer or \c UnkeyedDecodingContainer).
///
/// \param C The AST context to create the decl in.
///
/// \param DC The \c DeclContext to create the decl in.
///
/// \param unkeyedContainerDecl The generic type to bind the key type in.
///
/// \param introducer Whether to declare the variable as immutable.
///
/// \param name Name of the resulting variable
static VarDecl *createUnkeyedContainer(ASTContext &C, DeclContext *DC,
                                       NominalTypeDecl *unkeyedContainerDecl,
                                       VarDecl::Introducer introducer,
                                       Identifier name) {
  // let container : Keyed*Container<KeyType>
  auto *containerDecl =
      new (C) VarDecl(/*IsStatic=*/false, introducer, SourceLoc(), name, DC);
  containerDecl->setImplicit();
  containerDecl->setInterfaceType(
      unkeyedContainerDecl->getDeclaredInterfaceType());
  return containerDecl;
}

/// Creates a new \c CallExpr representing
///
///   base.container(keyedBy: CodingKeys.self)
///
/// \param C The AST context to create the expression in.
///
/// \param DC The \c DeclContext to create any decls in.
///
/// \param base The base expression to make the call on.
///
/// \param returnType The return type of the call.
///
/// \param param The parameter to the call.
static CallExpr *createContainerKeyedByCall(ASTContext &C, DeclContext *DC,
                                            Expr *base, Type returnType,
                                            NominalTypeDecl *param) {
  // (keyedBy:)
  auto *keyedByDecl = new (C)
      ParamDecl(SourceLoc(), SourceLoc(),
                C.Id_keyedBy, SourceLoc(), C.Id_keyedBy, DC);
  keyedByDecl->setImplicit();
  keyedByDecl->setSpecifier(ParamSpecifier::Default);
  keyedByDecl->setInterfaceType(returnType);

  // base.container(keyedBy:) expr
  auto *paramList = ParameterList::createWithoutLoc(keyedByDecl);
  auto *unboundCall = UnresolvedDotExpr::createImplicit(C, base, C.Id_container,
                                                        paramList);

  // CodingKeys.self expr
  auto *codingKeysExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), param, param->getDeclContext(),
      DC->mapTypeIntoContext(param->getInterfaceType()));
  auto *codingKeysMetaTypeExpr = new (C) DotSelfExpr(codingKeysExpr,
                                                     SourceLoc(), SourceLoc());

  // Full bound base.container(keyedBy: CodingKeys.self) call
  Expr *args[1] = {codingKeysMetaTypeExpr};
  Identifier argLabels[1] = {C.Id_keyedBy};
  return CallExpr::createImplicit(C, unboundCall, C.AllocateCopy(args),
                                  C.AllocateCopy(argLabels));
}

static CallExpr *createNestedContainerKeyedByForKeyCall(
    ASTContext &C, DeclContext *DC, Expr *base, NominalTypeDecl *codingKeysType,
    EnumElementDecl *key) {
  SmallVector<Identifier, 2> argNames{C.Id_keyedBy, C.Id_forKey};

  // base.nestedContainer(keyedBy:, forKey:) expr
  auto *unboundCall = UnresolvedDotExpr::createImplicit(
      C, base, C.Id_nestedContainer, argNames);

  // CodingKeys.self expr
  auto *codingKeysExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), codingKeysType, codingKeysType->getDeclContext(),
      DC->mapTypeIntoContext(codingKeysType->getInterfaceType()));
  auto *codingKeysMetaTypeExpr =
      new (C) DotSelfExpr(codingKeysExpr, SourceLoc(), SourceLoc());

  // key expr
  auto *metaTyRef = TypeExpr::createImplicit(
      DC->mapTypeIntoContext(key->getParentEnum()->getDeclaredInterfaceType()),
      C);
  auto *keyExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(), key,
                                        DeclNameLoc(), /*Implicit=*/true);

  // Full bound base.nestedContainer(keyedBy: CodingKeys.self, forKey: key) call
  Expr *args[2] = {codingKeysMetaTypeExpr, keyExpr};
  return CallExpr::createImplicit(C, unboundCall, C.AllocateCopy(args),
                                  argNames);
}

static CallExpr *createNestedUnkeyedContainerForKeyCall(ASTContext &C,
                                                        DeclContext *DC,
                                                        Expr *base,
                                                        Type returnType,
                                                        EnumElementDecl *key) {
  // (forKey:)
  auto *forKeyDecl = new (C) ParamDecl(SourceLoc(), SourceLoc(), C.Id_forKey,
                                       SourceLoc(), C.Id_forKey, DC);
  forKeyDecl->setImplicit();
  forKeyDecl->setSpecifier(ParamSpecifier::Default);
  forKeyDecl->setInterfaceType(returnType);

  // base.nestedUnkeyedContainer(forKey:) expr
  auto *paramList = ParameterList::createWithoutLoc(forKeyDecl);
  auto *unboundCall = UnresolvedDotExpr::createImplicit(
      C, base, C.Id_nestedUnkeyedContainer, paramList);

  // key expr
  auto *metaTyRef = TypeExpr::createImplicit(
      DC->mapTypeIntoContext(key->getParentEnum()->getDeclaredInterfaceType()),
      C);
  auto *keyExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(), key,
                                        DeclNameLoc(), /*Implicit=*/true);

  // Full bound base.nestedUnkeyedContainer(forKey: key) call
  return CallExpr::createImplicit(C, unboundCall, {keyExpr}, {C.Id_forKey});
}

/// Looks up the property corresponding to the indicated coding key.
///
/// \param conformanceDC The DeclContext we're generating code within.
/// \param elt The CodingKeys enum case.
/// \param targetDecl The type to look up properties in.
///
/// \return A tuple containing the \c VarDecl for the property, the type that
/// should be passed when decoding it, and a boolean which is true if
/// \c encodeIfPresent/\c decodeIfPresent should be used for this property.
static std::tuple<VarDecl *, Type, bool>
lookupVarDeclForCodingKeysCase(DeclContext *conformanceDC,
                               EnumElementDecl *elt,
                               NominalTypeDecl *targetDecl) {
  for (auto decl : targetDecl->lookupDirect(
                                   DeclName(elt->getBaseIdentifier()))) {
    if (auto *vd = dyn_cast<VarDecl>(decl)) {
      // If we found a property with an attached wrapper, retrieve the
      // backing property.
      if (auto backingVar = vd->getPropertyWrapperBackingProperty())
        vd = backingVar;

      if (!vd->isStatic()) {
        // This is the VarDecl we're looking for.

        auto varType =
            conformanceDC->mapTypeIntoContext(vd->getValueInterfaceType());

        bool useIfPresentVariant = false;

        if (auto objType = varType->getOptionalObjectType()) {
          varType = objType;
          useIfPresentVariant = true;
        }

        return std::make_tuple(vd, varType, useIfPresentVariant);
      }
    }
  }

  llvm_unreachable("Should have found at least 1 var decl");
}

static std::pair<BraceStmt *, bool>
deriveBodyEncodable_enum_encode(AbstractFunctionDecl *encodeDecl, void *) {
  // enum Foo : Codable {
  //   case bar(x: Int)
  //   case baz(y: String)
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case bar
  //     case baz
  //
  //     @derived enum CodingKeys_bar : CodingKey {
  //       case x
  //     }
  //
  //     @derived enum CodingKeys_baz : CodingKey {
  //       case y
  //     }
  //   }
  //
  //   @derived func encode(to encoder: Encoder) throws {
  //     var container = encoder.container(keyedBy: CodingKeys.self)
  //     switch self {
  //     case bar(let x):
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       CodingKeys_bar.self, forKey: .bar) try nestedContainer.encode(x,
  //       forKey: .x)
  //     case baz(let y):
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       CodingKeys_baz.self, forKey: .baz) try nestedContainer.encode(y,
  //       forKey: .y)
  //     }
  //   }
  // }

  // The enclosing type decl.
  auto conformanceDC = encodeDecl->getDeclContext();
  auto *enumDecl = conformanceDC->getSelfEnumDecl();

  auto *funcDC = cast<DeclContext>(encodeDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, enumDecl);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  SmallVector<ASTNode, 5> statements;

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to encode super.

  // let container : KeyedEncodingContainer<CodingKeys>
  auto *containerDecl =
      createKeyedContainer(C, funcDC, C.getKeyedEncodingContainerDecl(),
                           codingKeysEnum->getDeclaredInterfaceType(),
                           VarDecl::Introducer::Var, C.Id_container);

  auto *containerExpr =
      new (C) DeclRefExpr(ConcreteDeclRef(containerDecl), DeclNameLoc(),
                          /*Implicit=*/true, AccessSemantics::DirectToStorage);

  // Need to generate
  //   `let container = encoder.container(keyedBy: CodingKeys.self)`
  // This is unconditional because a type with no properties should encode as an
  // empty container.
  //
  // `let container` (containerExpr) is generated above.

  // encoder
  auto encoderParam = encodeDecl->getParameters()->get(0);
  auto *encoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(encoderParam),
                                          DeclNameLoc(), /*Implicit=*/true);

  // Bound encoder.container(keyedBy: CodingKeys.self) call
  auto containerType = containerDecl->getInterfaceType();
  auto *callExpr = createContainerKeyedByCall(C, funcDC, encoderExpr,
                                              containerType, codingKeysEnum);

  // Full `let container = encoder.container(keyedBy: CodingKeys.self)`
  // binding.
  auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
  auto *bindingDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, containerPattern, callExpr, funcDC);
  statements.push_back(bindingDecl);
  statements.push_back(containerDecl);

  auto *selfRef = encodeDecl->getImplicitSelfDecl();

  SmallVector<ASTNode, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    // CodingKeys.x
    auto *codingKeyCase =
        lookupEnumCase(C, codingKeysEnum, elt->getName().getBaseIdentifier());

    SmallVector<ASTNode, 3> caseStatements;

    // .<elt>(let a0, let a1, ...)
    SmallVector<VarDecl *, 3> payloadVars;
    auto subpattern = DerivedConformance::enumElementPayloadSubpattern(
        elt, 'a', encodeDecl, payloadVars, /* useLabels */ true);

    auto hasBoundDecls = !payloadVars.empty();
    Optional<MutableArrayRef<VarDecl *>> caseBodyVarDecls;
    if (hasBoundDecls) {
      // We allocated a direct copy of our var decls for the case
      // body.
      auto copy = C.Allocate<VarDecl *>(payloadVars.size());
      for (unsigned i : indices(payloadVars)) {
        auto *vOld = payloadVars[i];
        auto *vNew = new (C) VarDecl(
            /*IsStatic*/ false, vOld->getIntroducer(), vOld->getNameLoc(),
            vOld->getName(), vOld->getDeclContext());
        vNew->setImplicit();
        copy[i] = vNew;
      }
      caseBodyVarDecls.emplace(copy);
    }

    if (!codingKeyCase) {
      // This case should not be encodable, so throw an error if an attempt is
      // made to encode it
      // FIXME: throw EncodingError
      continue;
    } else if (elt->hasAnyUnnamedParameters()) {
      auto *nestedContainerDecl =
          createUnkeyedContainer(C, funcDC, C.getUnkeyedEncodingContainerDecl(),
                                 VarDecl::Introducer::Var,
                                 C.getIdentifier(StringRef("nestedContainer")));

      auto *nestedContainerExpr = new (C)
          DeclRefExpr(ConcreteDeclRef(nestedContainerDecl), DeclNameLoc(),
                      /*Implicit=*/true, AccessSemantics::DirectToStorage);
      auto *nestedContainerCall = createNestedUnkeyedContainerForKeyCall(
          C, funcDC, containerExpr, nestedContainerDecl->getInterfaceType(),
          codingKeyCase);

      auto *containerPattern =
          NamedPattern::createImplicit(C, nestedContainerDecl);
      auto *bindingDecl = PatternBindingDecl::createImplicit(
          C, StaticSpellingKind::None, containerPattern, nestedContainerCall,
          funcDC);

      caseStatements.push_back(bindingDecl);
      caseStatements.push_back(nestedContainerDecl);

      for (auto *payloadVar : payloadVars) {
        auto payloadVarRef = new (C) DeclRefExpr(payloadVar, DeclNameLoc(),
                                                 /*implicit*/ true);

        // encode(_:)
        auto *encodeCall = UnresolvedDotExpr::createImplicit(
            C, nestedContainerExpr, C.Id_encode, {Identifier()});

        // nestedContainer.encode(x)
        auto *callExpr = CallExpr::createImplicit(
            C, encodeCall, {payloadVarRef}, {Identifier()});

        // try nestedContainer.encode(x, forKey: CodingKeys.x)
        auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                        /*Implicit=*/true);
        caseStatements.push_back(tryExpr);
      }
    } else {
      auto caseIdentifier =
          combineIdentifiers(C, C.Id_CodingKeys, elt->getBaseIdentifier());
      auto *caseCodingKeys =
          lookupEvaluatedCodingKeysEnum(C, enumDecl, caseIdentifier);

      auto *nestedContainerDecl = createKeyedContainer(
          C, funcDC, C.getKeyedEncodingContainerDecl(),
          caseCodingKeys->getDeclaredInterfaceType(), VarDecl::Introducer::Var,
          C.getIdentifier(StringRef("nestedContainer")));

      auto *nestedContainerCall = createNestedContainerKeyedByForKeyCall(
          C, funcDC, containerExpr, caseCodingKeys, codingKeyCase);

      auto *containerPattern =
          NamedPattern::createImplicit(C, nestedContainerDecl);
      auto *bindingDecl = PatternBindingDecl::createImplicit(
          C, StaticSpellingKind::None, containerPattern, nestedContainerCall,
          funcDC);
      caseStatements.push_back(bindingDecl);
      caseStatements.push_back(nestedContainerDecl);

      for (auto *payloadVar : payloadVars) {
        auto *nestedContainerExpr = new (C)
            DeclRefExpr(ConcreteDeclRef(nestedContainerDecl), DeclNameLoc(),
                        /*Implicit=*/true, AccessSemantics::DirectToStorage);
        auto payloadVarRef = new (C) DeclRefExpr(payloadVar, DeclNameLoc(),
                                                 /*implicit*/ true);

        auto *caseCodingKey =
            lookupEnumCase(C, caseCodingKeys, payloadVar->getName());

        // If there is no key defined for this parameter, skip it.
        if (!caseCodingKey)
          continue;

        auto varType = conformanceDC->mapTypeIntoContext(
            payloadVar->getValueInterfaceType());

        bool useIfPresentVariant = false;
        if (auto objType = varType->getOptionalObjectType()) {
          varType = objType;
          useIfPresentVariant = true;
        }

        // CodingKeys_bar.x
        auto *metaTyRef =
            TypeExpr::createImplicit(caseCodingKeys->getDeclaredType(), C);
        auto *keyExpr =
            new (C) MemberRefExpr(metaTyRef, SourceLoc(), caseCodingKey,
                                  DeclNameLoc(), /*Implicit=*/true);

        // encode(_:forKey:)/encodeIfPresent(_:forKey:)
        auto methodName =
            useIfPresentVariant ? C.Id_encodeIfPresent : C.Id_encode;
        SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};

        auto *encodeCall = UnresolvedDotExpr::createImplicit(
            C, nestedContainerExpr, methodName, argNames);

        // nestedContainer.encode(x, forKey: CodingKeys.x)
        Expr *args[2] = {payloadVarRef, keyExpr};
        auto *callExpr = CallExpr::createImplicit(
            C, encodeCall, C.AllocateCopy(args), C.AllocateCopy(argNames));

        // try nestedContainer.encode(x, forKey: CodingKeys.x)
        auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                        /*Implicit=*/true);
        caseStatements.push_back(tryExpr);
      }
    }

    // generate: case .<Case>:
    auto pat = new (C) EnumElementPattern(
        TypeExpr::createImplicit(enumDecl->getDeclaredType(), C), SourceLoc(),
        DeclNameLoc(), DeclNameRef(), elt, subpattern);
    pat->setImplicit();

    auto labelItem = CaseLabelItem(pat);
    auto body = BraceStmt::create(C, SourceLoc(), caseStatements, SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     /*case body vardecls*/ caseBodyVarDecls));
  }

  // generate: switch self { }
  auto enumRef =
      new (C) DeclRefExpr(ConcreteDeclRef(selfRef), DeclNameLoc(),
                          /*implicit*/ true, AccessSemantics::Ordinary);

  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), enumRef,
                                       SourceLoc(), cases, SourceLoc(), C);
  statements.push_back(switchStmt);

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return {body, /*isTypeChecked=*/false};
}

/// Synthesizes the body for `func encode(to encoder: Encoder) throws`.
///
/// \param encodeDecl The function decl whose body to synthesize.
static std::pair<BraceStmt *, bool>
deriveBodyEncodable_encode(AbstractFunctionDecl *encodeDecl, void *) {
  // struct Foo : Codable {
  //   var x: Int
  //   var y: String
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case x
  //     case y
  //   }
  //
  //   @derived func encode(to encoder: Encoder) throws {
  //     var container = encoder.container(keyedBy: CodingKeys.self)
  //     try container.encode(x, forKey: .x)
  //     try container.encode(y, forKey: .y)
  //   }
  // }

  // The enclosing type decl.
  auto conformanceDC = encodeDecl->getDeclContext();
  auto *targetDecl = conformanceDC->getSelfNominalTypeDecl();

  auto *funcDC = cast<DeclContext>(encodeDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, targetDecl);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  SmallVector<ASTNode, 5> statements;

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to encode super.

  // let container : KeyedEncodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredType();
  auto *containerDecl =
      createKeyedContainer(C, funcDC, C.getKeyedEncodingContainerDecl(),
                           codingKeysEnum->getDeclaredInterfaceType(),
                           VarDecl::Introducer::Var, C.Id_container);

  auto *containerExpr = new (C) DeclRefExpr(ConcreteDeclRef(containerDecl),
                                            DeclNameLoc(), /*Implicit=*/true,
                                            AccessSemantics::DirectToStorage);

  // Need to generate
  //   `let container = encoder.container(keyedBy: CodingKeys.self)`
  // This is unconditional because a type with no properties should encode as an
  // empty container.
  //
  // `let container` (containerExpr) is generated above.

  // encoder
  auto encoderParam = encodeDecl->getParameters()->get(0);
  auto *encoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(encoderParam),
                                          DeclNameLoc(), /*Implicit=*/true);

  // Bound encoder.container(keyedBy: CodingKeys.self) call
  auto containerType = containerDecl->getInterfaceType();
  auto *callExpr = createContainerKeyedByCall(C, funcDC, encoderExpr,
                                              containerType, codingKeysEnum);

  // Full `let container = encoder.container(keyedBy: CodingKeys.self)`
  // binding.
  auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
  auto *bindingDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, containerPattern, callExpr, funcDC);
  statements.push_back(bindingDecl);
  statements.push_back(containerDecl);

  // Now need to generate `try container.encode(x, forKey: .x)` for all
  // existing properties. Optional properties get `encodeIfPresent`.
  for (auto *elt : codingKeysEnum->getAllElements()) {
    VarDecl *varDecl;
    Type varType;                // not used in Encodable synthesis
    bool useIfPresentVariant;

    std::tie(varDecl, varType, useIfPresentVariant) =
        lookupVarDeclForCodingKeysCase(conformanceDC, elt, targetDecl);

    // self.x
    auto *selfRef = DerivedConformance::createSelfDeclRef(encodeDecl);
    auto *varExpr =
        new (C) MemberRefExpr(selfRef, SourceLoc(), ConcreteDeclRef(varDecl),
                              DeclNameLoc(), /*Implicit=*/true);

    // CodingKeys.x
    auto *metaTyRef = TypeExpr::createImplicit(codingKeysType, C);
    auto *keyExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(), elt,
                                          DeclNameLoc(), /*Implicit=*/true);

    // encode(_:forKey:)/encodeIfPresent(_:forKey:)
    auto methodName = useIfPresentVariant ? C.Id_encodeIfPresent : C.Id_encode;
    SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};

    auto *encodeCall = UnresolvedDotExpr::createImplicit(C, containerExpr,
                                                         methodName, argNames);

    // container.encode(self.x, forKey: CodingKeys.x)
    Expr *args[2] = {varExpr, keyExpr};
    auto *callExpr = CallExpr::createImplicit(C, encodeCall,
                                              C.AllocateCopy(args),
                                              C.AllocateCopy(argNames));

    // try container.encode(self.x, forKey: CodingKeys.x)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*Implicit=*/true);
    statements.push_back(tryExpr);
  }

  // Classes which inherit from something Codable should encode super as well.
  if (superclassConformsTo(dyn_cast<ClassDecl>(targetDecl),
                           KnownProtocolKind::Encodable)) {
    // Need to generate `try super.encode(to: container.superEncoder())`

    // superEncoder()
    auto *method = UnresolvedDeclRefExpr::createImplicit(C, C.Id_superEncoder);

    // container.superEncoder()
    auto *superEncoderRef = new (C) DotSyntaxCallExpr(containerExpr,
                                                      SourceLoc(), method);

    // encode(to:) expr
    auto *encodeDeclRef = new (C) DeclRefExpr(ConcreteDeclRef(encodeDecl),
                                              DeclNameLoc(), /*Implicit=*/true);

    // super
    auto *superRef = new (C) SuperRefExpr(encodeDecl->getImplicitSelfDecl(),
                                          SourceLoc(), /*Implicit=*/true);

    // super.encode(to:)
    auto *encodeCall = new (C) DotSyntaxCallExpr(superRef, SourceLoc(),
                                                 encodeDeclRef);

    // super.encode(to: container.superEncoder())
    Expr *args[1] = {superEncoderRef};
    Identifier argLabels[1] = {C.Id_to};
    auto *callExpr = CallExpr::createImplicit(C, encodeCall,
                                              C.AllocateCopy(args),
                                              C.AllocateCopy(argLabels));

    // try super.encode(to: container.superEncoder())
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*Implicit=*/true);
    statements.push_back(tryExpr);
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return { body, /*isTypeChecked=*/false };
}

/// Synthesizes a function declaration for `encode(to: Encoder) throws` with a
/// lazily synthesized body for the given type.
///
/// Adds the function declaration to the given type before returning it.
static FuncDecl *deriveEncodable_encode(DerivedConformance &derived) {
  auto &C = derived.Context;
  auto conformanceDC = derived.getConformanceContext();
  auto *targetDecl = conformanceDC->getSelfNominalTypeDecl();

  // Expected type: (Self) -> (Encoder) throws -> ()
  // Constructed as: func type
  //                 input: Self
  //                 throws
  //                 output: function type
  //                         input: Encoder
  //                         output: ()
  // Create from the inside out:

  auto encoderType = C.getEncoderDecl()->getDeclaredInterfaceType();
  auto returnType = TupleType::getEmpty(C);

  // Params: (Encoder)
  auto *encoderParam = new (C)
      ParamDecl(SourceLoc(), SourceLoc(), C.Id_to,
                SourceLoc(), C.Id_encoder, conformanceDC);
  encoderParam->setSpecifier(ParamSpecifier::Default);
  encoderParam->setInterfaceType(encoderType);

  ParameterList *params = ParameterList::createWithoutLoc(encoderParam);

  // Func name: encode(to: Encoder)
  DeclName name(C, C.Id_encode, params);
  auto *const encodeDecl = FuncDecl::createImplicit(
      C, StaticSpellingKind::None, name, /*NameLoc=*/SourceLoc(),
      /*Async=*/false,
      /*Throws=*/true, /*GenericParams=*/nullptr, params, returnType,
      conformanceDC);
  encodeDecl->setSynthesized();

  if (auto *enumDecl = dyn_cast<EnumDecl>(targetDecl)) {
    // TODO: differentiate between cases
    encodeDecl->setBodySynthesizer(deriveBodyEncodable_enum_encode);
  } else {
    encodeDecl->setBodySynthesizer(deriveBodyEncodable_encode);
  }

  // This method should be marked as 'override' for classes inheriting Encodable
  // conformance from a parent class.
  if (superclassConformsTo(dyn_cast<ClassDecl>(derived.Nominal),
                           KnownProtocolKind::Encodable)) {
    auto *attr = new (C) OverrideAttr(/*IsImplicit=*/true);
    encodeDecl->getAttrs().add(attr);
  }

  encodeDecl->copyFormalAccessFrom(derived.Nominal,
                                   /*sourceIsParentContext*/ true);

  derived.addMembersToConformanceContext({encodeDecl});

  return encodeDecl;
}

/// Synthesizes the body for `init(from decoder: Decoder) throws`.
///
/// \param initDecl The function decl whose body to synthesize.
static std::pair<BraceStmt *, bool>
deriveBodyDecodable_enum_init(AbstractFunctionDecl *initDecl, void *) {
  // enum Foo : Codable {
  //   case bar(x: Int)
  //   case baz(y: String)
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case bar
  //     case baz
  //
  //     @derived enum CodingKeys_bar : CodingKey {
  //       case x
  //     }
  //
  //     @derived enum CodingKeys_baz : CodingKey {
  //       case y
  //     }
  //   }
  //
  //   @derived init(from decoder: Decoder) throws {
  //     let container = try decoder.container(keyedBy: CodingKeys.self)
  //     switch container.allKeys.first {
  //     case .bar:
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       CodingKeys_bar.self, forKey: .bar) let x = try
  //       nestedContainer.decode(Int.self, forKey: .x) self = .bar(x: x)
  //     case .baz:
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       CodingKeys_baz.self, forKey: .baz) let y = try
  //       nestedContainer.decode(String.self, forKey: .y) self = .baz(y: y)
  //     default:
  //       let context = DecodingError.Context(
  //         codingPath: container.codingPath,
  //         debugDescription: "Could not find value of type Foo")
  //       throw DecodingError.valueNotFound(Foo.self, context)
  //   }
  // }

  // The enclosing type decl.
  auto conformanceDC = initDecl->getDeclContext();
  auto *targetEnum = conformanceDC->getSelfEnumDecl();

  auto *funcDC = cast<DeclContext>(initDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, targetEnum);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to decode super.

  // let container : KeyedDecodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredInterfaceType();
  auto *containerDecl =
      createKeyedContainer(C, funcDC, C.getKeyedDecodingContainerDecl(),
                           codingKeysEnum->getDeclaredInterfaceType(),
                           VarDecl::Introducer::Let, C.Id_container);

  auto *containerExpr =
      new (C) DeclRefExpr(ConcreteDeclRef(containerDecl), DeclNameLoc(),
                          /*Implicit=*/true, AccessSemantics::DirectToStorage);

  SmallVector<ASTNode, 5> statements;
  if (codingKeysEnum->hasCases()) {
    // Need to generate
    //   `let container = try decoder.container(keyedBy: CodingKeys.self)`
    // `let container` (containerExpr) is generated above.

    // decoder
    auto decoderParam = initDecl->getParameters()->get(0);
    auto *decoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(decoderParam),
                                            DeclNameLoc(), /*Implicit=*/true);

    // Bound decoder.container(keyedBy: CodingKeys.self) call
    auto containerType = containerDecl->getInterfaceType();
    auto *callExpr = createContainerKeyedByCall(C, funcDC, decoderExpr,
                                                containerType, codingKeysEnum);

    // try decoder.container(keyedBy: CodingKeys.self)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*implicit=*/true);

    // Full `let container = decoder.container(keyedBy: CodingKeys.self)`
    // binding.
    auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
    auto *bindingDecl = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, containerPattern, tryExpr, funcDC);
    statements.push_back(bindingDecl);
    statements.push_back(containerDecl);

    SmallVector<ASTNode, 3> cases;

    for (auto *elt : targetEnum->getAllElements()) {
      auto *codingKeyCase =
          lookupEnumCase(C, codingKeysEnum, elt->getName().getBaseIdentifier());

      // Skip this case if it's not defined in the CodingKeys
      if (!codingKeyCase)
        continue;

      // generate: case .<Case>:
      auto pat = new (C) EnumElementPattern(
          TypeExpr::createImplicit(funcDC->mapTypeIntoContext(codingKeysType),
                                   C),
          SourceLoc(), DeclNameLoc(), DeclNameRef(), codingKeyCase, nullptr);
      pat->setImplicit();
      pat->setType(codingKeysType);

      auto labelItem =
          CaseLabelItem(new (C) OptionalSomePattern(pat, SourceLoc()));

      llvm::SmallVector<ASTNode, 3> caseStatements;
      if (!elt->hasAssociatedValues()) {
        // Foo.bar
        auto *selfTypeExpr =
            TypeExpr::createImplicit(targetEnum->getDeclaredType(), C);
        auto *selfCaseExpr = new (C) MemberRefExpr(
            selfTypeExpr, SourceLoc(), elt, DeclNameLoc(), /*Implicit=*/true);

        auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);

        auto *assignExpr =
            new (C) AssignExpr(selfRef, SourceLoc(), selfCaseExpr,
                               /*Implicit=*/true);

        caseStatements.push_back(assignExpr);
      } else if (elt->hasAnyUnnamedParameters()) {
        auto *nestedContainerDecl = createUnkeyedContainer(
            C, funcDC, C.getUnkeyedDecodingContainerDecl(),
            VarDecl::Introducer::Var,
            C.getIdentifier(StringRef("nestedContainer")));

        auto *nestedContainerCall = createNestedUnkeyedContainerForKeyCall(
            C, funcDC, containerExpr, nestedContainerDecl->getInterfaceType(),
            codingKeyCase);
        auto *tryNestedContainerCall = new (C) TryExpr(
            SourceLoc(), nestedContainerCall, Type(), /* Implicit */ true);

        auto *containerPattern =
            NamedPattern::createImplicit(C, nestedContainerDecl);
        auto *bindingDecl = PatternBindingDecl::createImplicit(
            C, StaticSpellingKind::None, containerPattern,
            tryNestedContainerCall, funcDC);

        caseStatements.push_back(bindingDecl);
        caseStatements.push_back(nestedContainerDecl);

        llvm::SmallVector<Expr *, 3> decodeCalls;
        llvm::SmallVector<Identifier, 3> params;
        for (auto *paramDecl : elt->getParameterList()->getArray()) {
          Identifier identifier = getVarNameForCoding(paramDecl);
          params.push_back(identifier);

          // Type.self
          auto *parameterTypeExpr =
              TypeExpr::createImplicit(paramDecl->getType(), C);
          auto *parameterMetaTypeExpr =
              new (C) DotSelfExpr(parameterTypeExpr, SourceLoc(), SourceLoc());
          auto *nestedContainerExpr = new (C)
              DeclRefExpr(ConcreteDeclRef(nestedContainerDecl), DeclNameLoc(),
                          /*Implicit=*/true, AccessSemantics::DirectToStorage);
          // decode(_:)
          auto *decodeCall = UnresolvedDotExpr::createImplicit(
              C, nestedContainerExpr, C.Id_decode, {Identifier()});

          // nestedContainer.decode(Type.self)
          auto *callExpr = CallExpr::createImplicit(
              C, decodeCall, {parameterMetaTypeExpr}, {Identifier()});

          // try nestedContainer.decode(Type.self)
          auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                          /*Implicit=*/true);

          decodeCalls.push_back(tryExpr);
        }

        auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);

        // Foo.bar
        auto *selfTypeExpr =
            TypeExpr::createImplicit(targetEnum->getDeclaredType(), C);

        // Foo.bar(x:)
        auto *selfCaseExpr = UnresolvedDotExpr::createImplicit(
            C, selfTypeExpr, elt->getBaseIdentifier(), C.AllocateCopy(params));

        // Foo.bar(x: try nestedContainer.decode(Int.self))
        auto *caseCallExpr = CallExpr::createImplicit(
            C, selfCaseExpr, C.AllocateCopy(decodeCalls),
            C.AllocateCopy(params));

        // self = Foo.bar(x: try nestedContainer.decode(Int.self))
        auto *assignExpr =
            new (C) AssignExpr(selfRef, SourceLoc(), caseCallExpr,
                               /*Implicit=*/true);

        caseStatements.push_back(assignExpr);
      } else {
        auto caseIdentifier =
            combineIdentifiers(C, C.Id_CodingKeys, elt->getBaseIdentifier());
        auto *caseCodingKeys =
            lookupEvaluatedCodingKeysEnum(C, targetEnum, caseIdentifier);

        auto *nestedContainerDecl =
            createKeyedContainer(C, funcDC, C.getKeyedDecodingContainerDecl(),
                                 caseCodingKeys->getDeclaredInterfaceType(),
                                 VarDecl::Introducer::Var,
                                 C.getIdentifier(StringRef("nestedContainer")));

        auto *nestedContainerCall = createNestedContainerKeyedByForKeyCall(
            C, funcDC, containerExpr, caseCodingKeys, codingKeyCase);

        auto *tryNestedContainerCall = new (C) TryExpr(
            SourceLoc(), nestedContainerCall, Type(), /* Implicit */ true);

        auto *containerPattern =
            NamedPattern::createImplicit(C, nestedContainerDecl);
        auto *bindingDecl = PatternBindingDecl::createImplicit(
            C, StaticSpellingKind::None, containerPattern,
            tryNestedContainerCall, funcDC);
        caseStatements.push_back(bindingDecl);
        caseStatements.push_back(nestedContainerDecl);

        llvm::SmallVector<Expr *, 3> decodeCalls;
        llvm::SmallVector<Identifier, 3> params;
        for (auto *paramDecl : elt->getParameterList()->getArray()) {
          auto *caseCodingKey = lookupEnumCase(
              C, caseCodingKeys, paramDecl->getBaseName().getIdentifier());

          Identifier identifier = getVarNameForCoding(paramDecl);
          params.push_back(identifier);

          // If no key is defined for this parameter, use the default value
          if (!caseCodingKey) {
            // FIXME: Better use DefaultArgumentExpr instead?
            // This should have been verified to have a default expr in the
            // CodingKey synthesis
            assert(paramDecl->hasDefaultExpr());
            decodeCalls.push_back(paramDecl->getTypeCheckedDefaultExpr());
            continue;
          }

          // Type.self
          auto *parameterTypeExpr =
              TypeExpr::createImplicit(paramDecl->getType(), C);
          auto *parameterMetaTypeExpr =
              new (C) DotSelfExpr(parameterTypeExpr, SourceLoc(), SourceLoc());
          // CodingKeys_bar.x
          auto *metaTyRef =
              TypeExpr::createImplicit(caseCodingKeys->getDeclaredType(), C);
          auto *keyExpr =
              new (C) MemberRefExpr(metaTyRef, SourceLoc(), caseCodingKey,
                                    DeclNameLoc(), /*Implicit=*/true);

          auto *nestedContainerExpr = new (C)
              DeclRefExpr(ConcreteDeclRef(nestedContainerDecl), DeclNameLoc(),
                          /*Implicit=*/true, AccessSemantics::DirectToStorage);
          // decode(_:, forKey:)
          auto *decodeCall = UnresolvedDotExpr::createImplicit(
              C, nestedContainerExpr, C.Id_decode, {Identifier(), C.Id_forKey});

          // nestedContainer.decode(Type.self, forKey: CodingKeys_bar.x)
          auto *callExpr = CallExpr::createImplicit(
              C, decodeCall, {parameterMetaTypeExpr, keyExpr},
              {Identifier(), C.Id_forKey});

          // try nestedContainer.decode(Type.self, forKey: CodingKeys_bar.x)
          auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                          /*Implicit=*/true);

          decodeCalls.push_back(tryExpr);
        }

        auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);

        // Foo.bar
        auto *selfTypeExpr =
            TypeExpr::createImplicit(targetEnum->getDeclaredType(), C);

        // Foo.bar(x:)
        auto *selfCaseExpr = UnresolvedDotExpr::createImplicit(
            C, selfTypeExpr, elt->getBaseIdentifier(), C.AllocateCopy(params));

        // Foo.bar(x: try nestedContainer.decode(Int.self, forKey: .x))
        auto *caseCallExpr = CallExpr::createImplicit(
            C, selfCaseExpr, C.AllocateCopy(decodeCalls),
            C.AllocateCopy(params));

        // self = Foo.bar(x: try nestedContainer.decode(Int.self))
        auto *assignExpr =
            new (C) AssignExpr(selfRef, SourceLoc(), caseCallExpr,
                               /*Implicit=*/true);

        caseStatements.push_back(assignExpr);
      }

      auto body =
          BraceStmt::create(C, SourceLoc(), caseStatements, SourceLoc());

      cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                       labelItem, SourceLoc(), SourceLoc(),
                                       body,
                                       /*case body vardecls*/ None));
    }

    auto *fatalErrorExpr =
        new (C) DeclRefExpr(C.getFatalError(), DeclNameLoc(), true);
    auto *errorMessage =
        new (C) StringLiteralExpr(StringRef("foo"), SourceRange(), true);
    auto *fatalErrorCall = CallExpr::createImplicit(
        C, fatalErrorExpr, {errorMessage}, {Identifier()});

    auto labelItem = CaseLabelItem::getDefault(AnyPattern::createImplicit(C));
    auto body =
        BraceStmt::create(C, SourceLoc(), ASTNode(fatalErrorCall), SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     /*case body vardecls*/ None));

    // generate: switch container.allKeys.first { }

    auto *allKeysExpr =
        UnresolvedDotExpr::createImplicit(C, containerExpr, C.Id_allKeys);

    auto *firstExpr =
        UnresolvedDotExpr::createImplicit(C, allKeysExpr, C.Id_first);

    auto switchStmt =
        SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), firstExpr,
                           SourceLoc(), cases, SourceLoc(), C);

    statements.push_back(switchStmt);
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return {body, /*isTypeChecked=*/false};
}

/// Synthesizes the body for `init(from decoder: Decoder) throws`.
///
/// \param initDecl The function decl whose body to synthesize.
static std::pair<BraceStmt *, bool>
deriveBodyDecodable_init(AbstractFunctionDecl *initDecl, void *) {
  // struct Foo : Codable {
  //   var x: Int
  //   var y: String
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case x
  //     case y
  //   }
  //
  //   @derived init(from decoder: Decoder) throws {
  //     let container = try decoder.container(keyedBy: CodingKeys.self)
  //     x = try container.decode(Type.self, forKey: .x)
  //     y = try container.decode(Type.self, forKey: .y)
  //   }
  // }

  // The enclosing type decl.
  auto conformanceDC = initDecl->getDeclContext();
  auto *targetDecl = conformanceDC->getSelfNominalTypeDecl();

  auto *funcDC = cast<DeclContext>(initDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, targetDecl);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to decode super.

  // let container : KeyedDecodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredType();
  auto *containerDecl =
      createKeyedContainer(C, funcDC, C.getKeyedDecodingContainerDecl(),
                           codingKeysEnum->getDeclaredInterfaceType(),
                           VarDecl::Introducer::Let, C.Id_container);

  auto *containerExpr = new (C) DeclRefExpr(ConcreteDeclRef(containerDecl),
                                            DeclNameLoc(), /*Implicit=*/true,
                                            AccessSemantics::DirectToStorage);

  SmallVector<ASTNode, 5> statements;
  auto enumElements = codingKeysEnum->getAllElements();
  if (!enumElements.empty()) {
    // Need to generate
    //   `let container = try decoder.container(keyedBy: CodingKeys.self)`
    // `let container` (containerExpr) is generated above.

    // decoder
    auto decoderParam = initDecl->getParameters()->get(0);
    auto *decoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(decoderParam),
                                            DeclNameLoc(), /*Implicit=*/true);

    // Bound decoder.container(keyedBy: CodingKeys.self) call
    auto containerType = containerDecl->getInterfaceType();
    auto *callExpr = createContainerKeyedByCall(C, funcDC, decoderExpr,
                                                containerType, codingKeysEnum);

    // try decoder.container(keyedBy: CodingKeys.self)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*implicit=*/true);

    // Full `let container = decoder.container(keyedBy: CodingKeys.self)`
    // binding.
    auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
    auto *bindingDecl = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, containerPattern, tryExpr, funcDC);
    statements.push_back(bindingDecl);
    statements.push_back(containerDecl);

    // Now need to generate `x = try container.decode(Type.self, forKey: .x)`
    // for all existing properties. Optional properties get `decodeIfPresent`.
    for (auto *elt : enumElements) {
      VarDecl *varDecl;
      Type varType;
      bool useIfPresentVariant;

      std::tie(varDecl, varType, useIfPresentVariant) =
          lookupVarDeclForCodingKeysCase(conformanceDC, elt, targetDecl);

      // Don't output a decode statement for a let with an initial value.
      if (varDecl->isLet() && varDecl->isParentInitialized()) {
        // But emit a warning to let the user know that it won't be decoded.
        auto lookupResult =
            codingKeysEnum->lookupDirect(varDecl->getBaseName());
        auto keyExistsInCodingKeys =
            llvm::any_of(lookupResult, [&](ValueDecl *VD) {
              if (isa<EnumElementDecl>(VD)) {
                return VD->getBaseName() == varDecl->getBaseName();
              }
              return false;
            });
        auto *encodableProto = C.getProtocol(KnownProtocolKind::Encodable);
        bool conformsToEncodable =
            conformanceDC->getParentModule()->lookupConformance(
                targetDecl->getDeclaredInterfaceType(), encodableProto) != nullptr;

        // Strategy to use for CodingKeys enum diagnostic part - this is to
        // make the behaviour more explicit:
        //
        // 1. If we have an *implicit* CodingKeys enum:
        // (a) If the type is Decodable only, explicitly define the enum and
        //     remove the key from it. This makes it explicit that the key
        //     will not be decoded.
        // (b) If the type is Codable, explicitly define the enum and keep the
        //     key in it. This is because removing the key will break encoding
        //     which is mostly likely not what the user expects.
        //
        // 2. If we have an *explicit* CodingKeys enum:
        // (a) If the type is Decodable only and the key exists in the enum,
        //     then explicitly remove the key from the enum. This makes it
        //     explicit that the key will not be decoded.
        // (b) If the type is Decodable only and the key does not exist in
        //     the enum, do nothing. This is because the user has explicitly
        //     made it clear that that they don't want the key to be decoded.
        // (c) If the type is Codable, do nothing. This is because removing
        //     the key will break encoding which is most likely not what the
        //     user expects.
        if (!codingKeysEnum->isImplicit()) {
          if (conformsToEncodable || !keyExistsInCodingKeys) {
            continue;
          }
        }

        varDecl->diagnose(diag::decodable_property_will_not_be_decoded);
        if (codingKeysEnum->isImplicit()) {
          varDecl->diagnose(
              diag::decodable_property_init_or_codingkeys_implicit,
              conformsToEncodable ? 0 : 1, varDecl->getName());
        } else {
          varDecl->diagnose(
              diag::decodable_property_init_or_codingkeys_explicit,
              varDecl->getName());
        }
        if (auto *PBD = varDecl->getParentPatternBinding()) {
          varDecl->diagnose(diag::decodable_make_property_mutable)
              .fixItReplace(PBD->getLoc(), "var");
        }

        continue;
      }

      auto methodName =
          useIfPresentVariant ? C.Id_decodeIfPresent : C.Id_decode;

      // Type.self (where Type === type(of: x))
      // Calculating the metatype needs to happen after potential Optional
      // unwrapping in lookupVarDeclForCodingKeysCase().
      auto *metaTyRef = TypeExpr::createImplicit(varType, C);
      auto *targetExpr = new (C) DotSelfExpr(metaTyRef, SourceLoc(),
                                             SourceLoc(), varType);

      // CodingKeys.x
      metaTyRef = TypeExpr::createImplicit(codingKeysType, C);
      auto *keyExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(),
                                            elt, DeclNameLoc(), /*Implicit=*/true);

      // decode(_:forKey:)/decodeIfPresent(_:forKey:)
      SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};
      auto *decodeCall = UnresolvedDotExpr::createImplicit(
          C, containerExpr, methodName, argNames);

      // container.decode(Type.self, forKey: CodingKeys.x)
      Expr *args[2] = {targetExpr, keyExpr};
      auto *callExpr = CallExpr::createImplicit(C, decodeCall,
                                                C.AllocateCopy(args),
                                                C.AllocateCopy(argNames));

      // try container.decode(Type.self, forKey: CodingKeys.x)
      auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                      /*Implicit=*/true);

      auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);
      auto *varExpr = UnresolvedDotExpr::createImplicit(C, selfRef,
                                                        varDecl->getName());
      auto *assignExpr = new (C) AssignExpr(varExpr, SourceLoc(), tryExpr,
                                            /*Implicit=*/true);
      statements.push_back(assignExpr);
    }
  }

  // Classes which have a superclass must call super.init(from:) if the
  // superclass is Decodable, or super.init() if it is not.
  if (auto *classDecl = dyn_cast<ClassDecl>(targetDecl)) {
    if (auto *superclassDecl = classDecl->getSuperclassDecl()) {
      if (superclassConformsTo(classDecl, KnownProtocolKind::Decodable)) {
        // Need to generate `try super.init(from: container.superDecoder())`

        // container.superDecoder
        auto *superDecoderRef =
          UnresolvedDotExpr::createImplicit(C, containerExpr,
                                            C.Id_superDecoder);

        // container.superDecoder()
        auto *superDecoderCall =
          CallExpr::createImplicit(C, superDecoderRef, ArrayRef<Expr *>(),
                                   ArrayRef<Identifier>());

        // super
        auto *superRef = new (C) SuperRefExpr(initDecl->getImplicitSelfDecl(),
                                              SourceLoc(), /*Implicit=*/true);

        // super.init(from:)
        auto *initCall = UnresolvedDotExpr::createImplicit(
            C, superRef, DeclBaseName::createConstructor(), {C.Id_from});

        // super.decode(from: container.superDecoder())
        Expr *args[1] = {superDecoderCall};
        Identifier argLabels[1] = {C.Id_from};
        auto *callExpr = CallExpr::createImplicit(C, initCall,
                                                  C.AllocateCopy(args),
                                                  C.AllocateCopy(argLabels));

        // try super.init(from: container.superDecoder())
        auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                        /*Implicit=*/true);
        statements.push_back(tryExpr);
      } else {
        // The explicit constructor name is a compound name taking no arguments.
        DeclName initName(C, DeclBaseName::createConstructor(),
                          ArrayRef<Identifier>());

        // We need to look this up in the superclass to see if it throws.
        auto result = superclassDecl->lookupDirect(initName);

        // We should have bailed one level up if this were not available.
        assert(!result.empty());

        // If the init is failable, we should have already bailed one level
        // above.
        ConstructorDecl *superInitDecl = cast<ConstructorDecl>(result.front());
        assert(!superInitDecl->isFailable());

        // super
        auto *superRef = new (C) SuperRefExpr(initDecl->getImplicitSelfDecl(),
                                              SourceLoc(), /*Implicit=*/true);

        // super.init()
        auto *superInitRef = UnresolvedDotExpr::createImplicit(C, superRef,
                                                               initName);
        // super.init() call
        Expr *callExpr = CallExpr::createImplicit(C, superInitRef,
                                                  ArrayRef<Expr *>(),
                                                  ArrayRef<Identifier>());

        // If super.init throws, try super.init()
        if (superInitDecl->hasThrows())
          callExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                     /*Implicit=*/true);

        statements.push_back(callExpr);
      }
    }
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return { body, /*isTypeChecked=*/false };
}

/// Synthesizes a function declaration for `init(from: Decoder) throws` with a
/// lazily synthesized body for the given type.
///
/// Adds the function declaration to the given type before returning it.
static ValueDecl *deriveDecodable_init(DerivedConformance &derived) {
  auto &C = derived.Context;

  auto classDecl = dyn_cast<ClassDecl>(derived.Nominal);
  auto conformanceDC = derived.getConformanceContext();

  // Expected type: (Self) -> (Decoder) throws -> (Self)
  // Constructed as: func type
  //                 input: Self
  //                 throws
  //                 output: function type
  //                         input: Encoder
  //                         output: Self
  // Compute from the inside out:

  // Params: (Decoder)
  auto decoderType = C.getDecoderDecl()->getDeclaredInterfaceType();
  auto *decoderParamDecl = new (C) ParamDecl(
      SourceLoc(), SourceLoc(), C.Id_from,
      SourceLoc(), C.Id_decoder, conformanceDC);
  decoderParamDecl->setImplicit();
  decoderParamDecl->setSpecifier(ParamSpecifier::Default);
  decoderParamDecl->setInterfaceType(decoderType);

  auto *paramList = ParameterList::createWithoutLoc(decoderParamDecl);

  // Func name: init(from: Decoder)
  DeclName name(C, DeclBaseName::createConstructor(), paramList);

  auto *initDecl =
      new (C) ConstructorDecl(name, SourceLoc(),
                              /*Failable=*/false,SourceLoc(),
                              /*Throws=*/true, SourceLoc(), paramList,
                              /*GenericParams=*/nullptr, conformanceDC);
  initDecl->setImplicit();
  initDecl->setSynthesized();
  if (auto enumDecl = dyn_cast<EnumDecl>(derived.Nominal)) {
    initDecl->setBodySynthesizer(&deriveBodyDecodable_enum_init);
  } else {
    initDecl->setBodySynthesizer(&deriveBodyDecodable_init);
  }

  // This constructor should be marked as `required` for non-final classes.
  if (classDecl && !classDecl->isFinal()) {
    auto *reqAttr = new (C) RequiredAttr(/*IsImplicit=*/true);
    initDecl->getAttrs().add(reqAttr);
  }

  initDecl->copyFormalAccessFrom(derived.Nominal,
                                 /*sourceIsParentContext*/ true);

  derived.addMembersToConformanceContext({initDecl});

  return initDecl;
}

/// Returns whether the given type is valid for synthesizing {En,De}codable.
///
/// Checks to see whether the given type has a valid \c CodingKeys enum, and if
/// not, attempts to synthesize one for it.
///
/// \param requirement The requirement we want to synthesize.
static bool canSynthesize(DerivedConformance &derived, ValueDecl *requirement) {
  // Before we attempt to look up (or more importantly, synthesize) a CodingKeys
  // entity on target, we need to make sure the type is otherwise valid.
  //
  // If we are synthesizing Decodable and the target is a class with a
  // superclass, our synthesized init(from:) will need to call either
  // super.init(from:) or super.init() depending on whether the superclass is
  // Decodable itself.
  //
  // If the required initializer is not available, we shouldn't attempt to
  // synthesize CodingKeys.
  auto proto = derived.Protocol;
  auto *classDecl = dyn_cast<ClassDecl>(derived.Nominal);
  if (proto->isSpecificProtocol(KnownProtocolKind::Decodable) && classDecl) {
    if (auto *superclassDecl = classDecl->getSuperclassDecl()) {
      DeclName memberName;
      auto superType = superclassDecl->getDeclaredInterfaceType();
      if (TypeChecker::conformsToProtocol(superType, proto, superclassDecl)) {
        // super.init(from:) must be accessible.
        memberName = cast<ConstructorDecl>(requirement)->getName();
      } else {
        // super.init() must be accessible.
        // Passing an empty params array constructs a compound name with no
        // arguments (as opposed to a simple name when omitted).
        memberName =
            DeclName(derived.Context, DeclBaseName::createConstructor(),
                     ArrayRef<Identifier>());
      }

      auto result =
          TypeChecker::lookupMember(superclassDecl, superType,
                                    DeclNameRef(memberName));

      if (result.empty()) {
        // No super initializer for us to call.
        superclassDecl->diagnose(diag::decodable_no_super_init_here,
                                 requirement->getName(), memberName);
        return false;
      } else if (result.size() > 1) {
        // There are multiple results for this lookup. We'll end up producing a
        // diagnostic later complaining about duplicate methods (if we haven't
        // already), so just bail with a general error.
        return false;
      } else {
        auto *initializer =
          cast<ConstructorDecl>(result.front().getValueDecl());
        auto conformanceDC = derived.getConformanceContext();
        if (!initializer->isDesignatedInit()) {
          // We must call a superclass's designated initializer.
          initializer->diagnose(diag::decodable_super_init_not_designated_here,
                                requirement->getName(), memberName);
          return false;
        } else if (!initializer->isAccessibleFrom(conformanceDC)) {
          // Cannot call an inaccessible method.
          auto accessScope = initializer->getFormalAccessScope(conformanceDC);
          initializer->diagnose(diag::decodable_inaccessible_super_init_here,
                                requirement->getName(), memberName,
                                accessScope.accessLevelForDiagnostics());
          return false;
        } else if (initializer->isFailable()) {
          // We can't call super.init() if it's failable, since init(from:)
          // isn't failable.
          initializer->diagnose(diag::decodable_super_init_is_failable_here,
                                requirement->getName(), memberName);
          return false;
        }
      }
    }
  }

  switch (classifyCodingKeys(derived)) {
  case CodingKeysClassification::Invalid:
    return false;
  case CodingKeysClassification::NeedsSynthesizedCodingKeys:
    return synthesizeCodingKeysEnum(derived);
  case CodingKeysClassification::Valid:
    return true;
  }
}

ValueDecl *DerivedConformance::deriveEncodable(ValueDecl *requirement) {
  // We can only synthesize Encodable for structs and classes.
  if (!isa<StructDecl>(Nominal) && !isa<ClassDecl>(Nominal) &&
      !isa<EnumDecl>(Nominal))
    return nullptr;

  if (requirement->getBaseName() != Context.Id_encode) {
    // Unknown requirement.
    requirement->diagnose(diag::broken_encodable_requirement);
    return nullptr;
  }

  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;

  // We're about to try to synthesize Encodable. If something goes wrong,
  // we'll have to output at least one error diagnostic because we returned
  // true from NominalTypeDecl::derivesProtocolConformance; if we don't, we're
  // expected to return a witness here later (and we crash on an assertion).
  // Producing a diagnostic stops compilation before then.
  //
  // A synthesis attempt will produce NOTE diagnostics throughout, but we'll
  // want to collect them before displaying -- we want NOTEs to display
  // _after_ a main diagnostic so we don't get a NOTE before the error it
  // relates to.
  //
  // We can do this with a diagnostic transaction -- first collect failure
  // diagnostics, then potentially collect notes. If we succeed in
  // synthesizing Encodable, we can cancel the transaction and get rid of the
  // fake failures.
  DiagnosticTransaction diagnosticTransaction(Context.Diags);
  ConformanceDecl->diagnose(diag::type_does_not_conform,
                            Nominal->getDeclaredType(), getProtocolType());
  requirement->diagnose(diag::no_witnesses, diag::RequirementKind::Func,
                        requirement->getName(), getProtocolType(),
                        /*AddFixIt=*/false);

  // Check other preconditions for synthesized conformance.
  // This synthesizes a CodingKeys enum if possible.
  if (canSynthesize(*this, requirement)) {
    diagnosticTransaction.abort();
    return deriveEncodable_encode(*this);
  }

  return nullptr;
}

ValueDecl *DerivedConformance::deriveDecodable(ValueDecl *requirement) {
  // We can only synthesize Encodable for structs and classes.
  if (!isa<StructDecl>(Nominal) && !isa<ClassDecl>(Nominal) &&
      !isa<EnumDecl>(Nominal))
    return nullptr;

  if (requirement->getBaseName() != DeclBaseName::createConstructor()) {
    // Unknown requirement.
    requirement->diagnose(diag::broken_decodable_requirement);
    return nullptr;
  }

  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;

  // We're about to try to synthesize Decodable. If something goes wrong,
  // we'll have to output at least one error diagnostic. We need to collate
  // diagnostics produced by canSynthesize and deriveDecodable_init to produce
  // them in the right order -- see the comment in deriveEncodable for
  // background on this transaction.
  DiagnosticTransaction diagnosticTransaction(Context.Diags);
  ConformanceDecl->diagnose(diag::type_does_not_conform,
                            Nominal->getDeclaredType(), getProtocolType());
  requirement->diagnose(diag::no_witnesses, diag::RequirementKind::Constructor,
                        requirement->getName(), getProtocolType(),
                        /*AddFixIt=*/false);

  // Check other preconditions for synthesized conformance.
  // This synthesizes a CodingKeys enum if possible.
  if (canSynthesize(*this, requirement)) {
    diagnosticTransaction.abort();
    return deriveDecodable_init(*this);
  }

  return nullptr;
}
