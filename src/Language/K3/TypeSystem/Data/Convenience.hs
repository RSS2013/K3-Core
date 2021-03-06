{-# LANGUAGE TemplateHaskell, MultiParamTypeClasses, FlexibleInstances #-}

{-|
  A module containing convenience definitions for users of the type system.
-}
module Language.K3.TypeSystem.Data.Convenience
( NormalQuantType
, NormalAnnType
, NormalAnnBodyType
, NormalAnnMemType
, emptyAnnotation
, ConstraintConstructor2(..)
, (<:)
, constraintEquiv
, (~=)
) where

import Control.Applicative
import qualified Data.Map as Map
import Data.Set (Set)

import qualified Language.K3.TypeSystem.ConstraintSetLike as CSL
import Language.K3.TypeSystem.Data.Constraints
import Language.K3.TypeSystem.Data.ConstraintSet
import Language.K3.TypeSystem.Data.Coproduct
import Language.K3.TypeSystem.Data.Types

-- |A type alias for normal quantified types (which use normal constraint sets).
type NormalQuantType = QuantType ConstraintSet

-- |A type alias for normal annotation types (which use normal constraint sets).
type NormalAnnType = AnnType ConstraintSet

-- |An alias for normal annotation body types.
type NormalAnnBodyType = AnnBodyType ConstraintSet

-- |An alias for normal annotation member types.
type NormalAnnMemType = AnnMemType ConstraintSet

-- |A value defining the empty annotation.
emptyAnnotation :: NormalAnnType
emptyAnnotation = AnnType Map.empty (AnnBodyType [] []) csEmpty
  
-- |A typeclass with convenience instances for constructing constraints.  This
--  constructor only works on 2-ary constraint constructors (which most
--  constraints have).
class ConstraintConstructor2 a b where
  constraint :: a -> b -> Constraint
  
-- |An infix synonym for @constraint@.
infix 7 <:
(<:) :: (ConstraintConstructor2 a b) => a -> b -> Constraint
(<:) = constraint

-- |A function which generates a constraint equivalence.
constraintEquiv :: ( ConstraintConstructor2 a b
                   , ConstraintConstructor2 b a
                   , CSL.ConstraintSetLike e c)
                => a -> b -> c
constraintEquiv x y =
  CSL.union (CSL.csingleton $ x <: y) (CSL.csingleton $ y <: x)

-- |An infix synonym for @constraintEquiv@.
infix 7 ~=
(~=) :: ( ConstraintConstructor2 a b
        , ConstraintConstructor2 b a
        , CSL.ConstraintSetLike e c)
     => a -> b -> c
(~=) = constraintEquiv

{-
  The following Template Haskell creates various instances for the constraint
  function.  In each case of a TypeOrVar or QualOrVar, three variations are
  produced: one which takes the left side, one which takes the right, and one
  which takes the actual Either structure.
-}
$(
  -- The instances variable contains 5-tuples describing the varying positions
  -- in the typeclass instance template below.
  let instances =
        let typeOrVar = [ ([t|ShallowType|], [|CLeft|])
                        , ([t|UVar|], [|CRight|])
                        , ([t|TypeOrVar|], [|id|])
                        ]
            qualOrVar = [ ([t|Set TQual|], [|CLeft|])
                        , ([t|QVar|], [|CRight|])
                        , ([t|QualOrVar|], [|id|])
                        ]
        in
        -- Each of the following lists represent the 5-tuples for one constraint
        -- constructor.
        [ (t1, t2, [|IntermediateConstraint|], f1, f2)
        | (t1,f1) <- typeOrVar
        , (t2,f2) <- typeOrVar
        ]
        ++
        [ (t1, [t|QVar|], [|QualifiedLowerConstraint|], f1, [|id|])
        | (t1, f1) <- typeOrVar
        ]
        ++
        [ ([t|QVar|], t2, [|QualifiedUpperConstraint|], [|id|], f2)
        | (t2, f2) <- typeOrVar
        ]
        ++
        [ (t1, t2, [|QualifiedIntermediateConstraint|], f1, f2)
        | (t1, f1) <- qualOrVar
        , (t2, f2) <- qualOrVar
        ]
  in
  -- This function takes the 5-tuples and feeds them into the typeclass instance
  -- template, the actual instances.
  let mkInstance (t1, t2, cons, f1, f2) =
        [d|
          instance ConstraintConstructor2 $t1 $t2 where
            constraint a b = $cons ($f1 a) ($f2 b)
        |]
  in
  -- Rubber, meet road.
  concat <$> mapM mkInstance instances
 )
  