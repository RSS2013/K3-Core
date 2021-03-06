module Language.K3.TypeSystem.Test
( tests
) where

import Control.Applicative
import Control.Monad
import qualified Data.Foldable as Foldable
import Data.List
import qualified Data.Map as Map
import Data.Maybe
import qualified Data.Sequence as Seq
import System.Directory
import System.FilePath
import Test.HUnit hiding (Test)
import Test.Framework.Providers.API
import Test.Framework.Providers.HUnit
import Text.Parsec.Error
import Text.Parsec.Prim as PPrim

import Language.K3.Core.Annotation
import Language.K3.Core.Constructor.Declaration
import Language.K3.Core.Declaration
import Language.K3.Parser
import Language.K3.TypeSystem
import Language.K3.Utils.Pretty

import Debug.Trace

-- |Generates tests for the type system.  If a string is provided, only files
--  with precisely that name will be tested.
tests :: Maybe String -> IO [Test]
tests mfilename =
  concat <$> sequence
    [ mkTests "Typecheck" True "success"
    , mkTests "Type fail" False "failure"
    ]
  where
    mkTests :: String -> Bool -> FilePath -> IO [Test]
    mkTests name success subdir =
      let prefix = testFilePath </> subdir in
      let files = filter testPredicate <$> getDirectoryContents prefix in
      sequence    
        [
          testGroup name <$>
            map (\path -> testCase path $ mkDirectSourceTest path success) <$>
            map (prefix </>) <$> files
        ]
    testPredicate name = (".k3" `isSuffixOf` name) &&
                         case mfilename of
                            Nothing -> True
                            Just filename -> filename == name

testFilePath :: FilePath
testFilePath = "examples" </> "typeSystem"
  
-- |This function, when given the path of an example source file, will generate
--  a test to parse and typecheck it.  The parsed code is submitted directly to
--  the type system; it is not preprocessed in any way.
mkDirectSourceTest :: FilePath -> Bool -> Assertion
mkDirectSourceTest path success = do
  src <- readFile path
  case parseSource path src of
    Left err -> assertFailure $ "Parse failure: " ++ show err
    Right decl ->
      case ( Foldable.toList $ fst $
                typecheck Map.empty Map.empty Map.empty decl
           , success) of
        ([], True) -> assert True
        (errs@(_:_), True) ->
          assertFailure $ "Typechecking errors: " ++ pretty errs
        ([], False) -> assert "Incorrectly typechecked!"
        (_:_, False) -> assert True

-- |Parses a top-level source file in K3 *without* processing the AST for
--  program generation and the like.
parseSource :: String -> String -> Either ParseError (K3 Declaration)
parseSource name src =
  let parser = join $ mapM ensureUIDs <$>
                  concat <$> PPrim.many declaration in
  let tree = role "__global" <$> runParser parser (0,[]) name src in
  tree
