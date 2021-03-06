{-# LANGUAGE TupleSections, DataKinds #-}

{-|
  This module contains general type manipulation utilities.
-}
module Language.K3.TypeSystem.Utils
( typeOfOp
, typeOfPol
, recordConcat
, RecordConcatenationError(..)
, getLowerBoundsOf
, getUpperBoundsOf
, leastFixedPoint
, leastFixedPointM
, quasiFreshVar
) where

import Control.Monad
import Data.List
import Data.Map as Map
import Data.Set as Set

import Language.K3.Core.Common
import Language.K3.Core.Declaration
import Language.K3.Core.Expression
import Language.K3.TypeSystem.Data

-- |Translates from expression-level operators to types.
typeOfOp :: Operator -> AnyOperator
typeOfOp op = case op of
  OAdd -> SomeBinaryOperator BinOpAdd
  OSub -> SomeBinaryOperator BinOpSubtract
  OMul -> SomeBinaryOperator BinOpMultiply
  ODiv -> SomeBinaryOperator BinOpDivide
  ONeg -> error "No unary operators in spec yet!" -- TODO: unary operators specification
  OEqu -> SomeBinaryOperator BinOpEquals
  ONeq -> error "No not-equals in spec yet!" -- TODO: unary operators specification
  OLth -> SomeBinaryOperator BinOpLess
  OLeq -> SomeBinaryOperator BinOpLessEq
  OGth -> SomeBinaryOperator BinOpGreater
  OGeq -> SomeBinaryOperator BinOpGreaterEq
  OAnd -> error "No and in spec yet!" -- TODO: binary logic operators in specification
  OOr -> error "No or in spec yet!" -- TODO: binary logic operators in specification
  ONot -> error "No unary operators in spec yet!" -- TODO: unary operators in specification
  OConcat -> SomeBinaryOperator BinOpConcat
  OSeq -> SomeBinaryOperator BinOpSequence
  OApp -> SomeBinaryOperator BinOpApply
  OSnd -> SomeBinaryOperator BinOpSend
  
-- |Translates from expression-level polarities.
typeOfPol :: Polarity -> TPolarity
typeOfPol Provides = Positive
typeOfPol Requires = Negative

data RecordConcatenationError
  = RecordIdentifierOverlap (Set Identifier)
  | RecordOpaqueOverlap (Set OpaqueVar)
  | NonRecordType ShallowType
  deriving (Eq, Show)

-- |Concatenates a set of concrete record types.  A @Nothing@ is produced if
--  any of the types are not records (or top) or if the record types overlap.
recordConcat :: [ShallowType] -> Either RecordConcatenationError ShallowType
recordConcat = foldM concatRecs (SRecord Map.empty Set.empty) .
                  Prelude.filter (/=STop)
  where
    -- |A function to concatenate two record types.  The first argument is the
    --  record type accumulated so far; the second argument is a record type to
    --  include.  The result is either a new record type or an error in
    --  concatenation.
    concatRecs :: ShallowType -> ShallowType
               -> Either RecordConcatenationError ShallowType
    concatRecs t1 t2 =
      case (t1,t2) of
        (SRecord m1 oas1, SRecord m2 oas2) ->
          let labelOverlap = Set.fromList (Map.keys m1) `Set.intersection`
                             Set.fromList (Map.keys m2) in
          let opaqueOverlap = oas1 `Set.intersection` oas2 in
          if Set.null labelOverlap
            then
              if Set.null opaqueOverlap
                then Right $ SRecord (m1 `Map.union` m2) (oas1 `Set.union` oas2)
                else Left $ RecordOpaqueOverlap opaqueOverlap
            else Left $ RecordIdentifierOverlap labelOverlap
        (SRecord _ _, _) -> Left $ NonRecordType t2
        (_, _) -> Left $ NonRecordType t1

-- |Calculates the upper bounds of a given @TypeOrVar@ in context of a
--  constraint set.  If the argument is a type, that type is returned.
--  Otherwise, the argument's upper bounds are returned.
getLowerBoundsOf :: ConstraintSet -> TypeOrVar -> [ShallowType]
getLowerBoundsOf cs ta =
  case ta of
    CLeft t -> [t]
    CRight a -> nub $ csQuery cs $ QueryTypeByUVarUpperBound a

-- |Calculates the upper bounds of a given @TypeOrVar@ in context of a
--  constraint set.  If the argument is a type, that type is returned.
--  Otherwise, the argument's upper bounds are returned.
getUpperBoundsOf :: ConstraintSet -> TypeOrVar -> [ShallowType]
getUpperBoundsOf cs ta =
  case ta of
    CLeft t -> [t]
    CRight a -> nub $ csQuery cs $ QueryTypeByUVarLowerBound a

-- |Calculates the least fixed point of an operation given a value.
leastFixedPoint :: (Eq a) => (a -> a) -> a -> a
leastFixedPoint f x =
  let xs = iterate f x in
  let pairs = zip xs $ tail xs in
  snd $ head $ Data.List.filter (uncurry (==)) pairs
  
-- |Calculates the least fixed point of a monadic operation given a value.  Note
--  that the behavior of the monad is not taken into account; this operation
--  will stop iterating once the value does not change regardless of the monad's
--  behavior.  The result will be the element which was most recently computed.
leastFixedPointM :: (Monad m, Eq a) => (a -> m a) -> a -> m a
leastFixedPointM f x = do
  x' <- f x
  if x == x'
    then return x'
    else leastFixedPointM f x'

-- TODO: get rid of quasi-fresh stuff; it's no longer used

-- |A class defining ad-hoc overloading for a routine which generates a
--  quasi-fresh variable.
class QuasiFreshVarConstruction q where
  quasiFreshVar :: TVar q' -> TVarQuasiFreshIndex -> TVar q

instance QuasiFreshVarConstruction UnqualifiedTVar where
  quasiFreshVar = quasiFreshVar' UTVar

instance QuasiFreshVarConstruction QualifiedTVar where
  quasiFreshVar = quasiFreshVar' QTVar

quasiFreshVar' :: (TVarID -> TVarOrigin q -> TVar q)
               -> TVar q' -> TVarQuasiFreshIndex -> TVar q
quasiFreshVar' constr v idx =
  constr (TVarQuasiFreshID (tvarId v) idx)
         (TVarQuasiFreshOrigin (someVar v) idx)
