#pragma once

#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeFunction.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnFunction.h>
#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>
#include <Functions/IFunctionImpl.h>
#include <Functions/FunctionHelpers.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context_fwd.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int SIZES_OF_ARRAYS_DOESNT_MATCH;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}


/** Higher-order functions for arrays.
  * These functions optionally apply a map (transform) to array (or multiple arrays of identical size) by lambda function,
  *  and return some result based on that transformation.
  *
  * Examples:
  * arrayMap(x1,...,xn -> expression, array1,...,arrayn) - apply the expression to each element of the array (or set of parallel arrays).
  * arrayFilter(x -> predicate, array) - leave in the array only the elements for which the expression is true.
  *
  * For some functions arrayCount, arrayExists, arrayAll, an overload of the form f(array) is available,
  *  which works in the same way as f(x -> x, array).
  *
  * See the example of Impl template parameter in arrayMap.cpp
  */
template <typename Impl, typename Name>
class FunctionArrayMapped : public IFunction
{
public:
    static constexpr auto name = Name::name;
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionArrayMapped>(); }

    String getName() const override
    {
        return name;
    }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    /// Called if at least one function argument is a lambda expression.
    /// For argument-lambda expressions, it defines the types of arguments of these expressions.
    void getLambdaArgumentTypes(DataTypes & arguments) const override
    {
        if (arguments.empty())
            throw Exception("Function " + getName() + " needs at least one argument; passed "
                            + toString(arguments.size()) + ".",
                            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        if (arguments.size() == 1)
            throw Exception("Function " + getName() + " needs at least one array argument.",
                            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        size_t arguments_to_skip = Impl::isFolding() ? 1 : 0;
        DataTypes nested_types(arguments.size() - 1);
        for (size_t i = 0; i < nested_types.size() - arguments_to_skip; ++i)
        {
            const DataTypeArray * array_type = checkAndGetDataType<DataTypeArray>(&*arguments[i + 1]);
            if (!array_type)
                throw Exception("Argument " + toString(i + 2) + " of function " + getName() + " must be array. Found "
                                + arguments[i + 1]->getName() + " instead.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            nested_types[i] = recursiveRemoveLowCardinality(array_type->getNestedType());
        }
        if (Impl::isFolding())
            nested_types[nested_types.size() - 1] = arguments[arguments.size() - 1];

        const DataTypeFunction * function_type = checkAndGetDataType<DataTypeFunction>(arguments[0].get());
        if (!function_type || function_type->getArgumentTypes().size() != nested_types.size())
            throw Exception("First argument for this overload of " + getName() + " must be a function with "
                            + toString(nested_types.size()) + " arguments. Found "
                            + arguments[0]->getName() + " instead.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

        arguments[0] = std::make_shared<DataTypeFunction>(nested_types);
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        size_t min_args = Impl::needExpression() ? 2 : 1;
        if (arguments.size() < min_args)
            throw Exception("Function " + getName() + " needs at least "
                            + toString(min_args) + " argument; passed "
                            + toString(arguments.size()) + ".",
                            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        if (arguments.size() == 1)
        {
            const auto * array_type = checkAndGetDataType<DataTypeArray>(arguments[0].type.get());

            if (!array_type)
                throw Exception("The only argument for function " + getName() + " must be array. Found "
                                + arguments[0].type->getName() + " instead.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            DataTypePtr nested_type = array_type->getNestedType();

            if (Impl::needBoolean() && !WhichDataType(nested_type).isUInt8())
                throw Exception("The only argument for function " + getName() + " must be array of UInt8. Found "
                                + arguments[0].type->getName() + " instead.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            return Impl::getReturnType(nested_type, nested_type);
        }
        else
        {
            if (arguments.size() > 2 && Impl::needOneArray())
                throw Exception("Function " + getName() + " needs one array argument.",
                    ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

            const auto * data_type_function = checkAndGetDataType<DataTypeFunction>(arguments[0].type.get());

            if (!data_type_function)
                throw Exception("First argument for function " + getName() + " must be a function.",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            /// The types of the remaining arguments are already checked in getLambdaArgumentTypes.

            DataTypePtr return_type = removeLowCardinality(data_type_function->getReturnType());
            if (Impl::needBoolean() && !WhichDataType(return_type).isUInt8())
                throw Exception("Expression for function " + getName() + " must return UInt8, found "
                                + return_type->getName(), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            if (Impl::isFolding())
            {
                const auto accum_type = arguments.back().type;
                return Impl::getReturnType(return_type, accum_type);
            }
            else
            {
                const auto * first_array_type = checkAndGetDataType<DataTypeArray>(arguments[1].type.get());
                return Impl::getReturnType(return_type, first_array_type->getNestedType());
            }
        }
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t /*input_rows_count*/) const override
    {
        std::cerr << " *** FOLDING" << std::endl;
        std::cerr << "  isFolding(): " << (Impl::isFolding() ? "yes" : "no-") << std::endl;
        std::cerr << "  arguments.size() = " << arguments.size() << std::endl;

        if (arguments.size() == 1)
        {
            ColumnPtr column_array_ptr = arguments[0].column;
            const auto * column_array = checkAndGetColumn<ColumnArray>(column_array_ptr.get());

            if (!column_array)
            {
                const ColumnConst * column_const_array = checkAndGetColumnConst<ColumnArray>(column_array_ptr.get());
                if (!column_const_array)
                    throw Exception("X1 Expected array column, found " + column_array_ptr->getName(), ErrorCodes::ILLEGAL_COLUMN);
                column_array_ptr = column_const_array->convertToFullColumn();
                column_array = assert_cast<const ColumnArray *>(column_array_ptr.get());
            }

            return Impl::execute(*column_array, column_array->getDataPtr());
        }
        else
        {
            const auto & column_with_type_and_name = arguments[0];

            if (!column_with_type_and_name.column)
                throw Exception("First argument for function " + getName() + " must be a function.",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            const auto * column_function = typeid_cast<const ColumnFunction *>(column_with_type_and_name.column.get());

            if (!column_function)
                throw Exception("First argument for function " + getName() + " must be a function.",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            ColumnPtr offsets_column;

            ColumnPtr column_first_array_ptr;
            const ColumnArray * column_first_array = nullptr;

            size_t arguments_to_skip = Impl::isFolding() ? 1 : 0;

            ColumnsWithTypeAndName arrays;
            arrays.reserve(arguments.size() - 1);

            for (size_t i = 1; i < arguments.size() - arguments_to_skip; ++i)
            {
                const auto & array_with_type_and_name = arguments[i];

                ColumnPtr column_array_ptr = array_with_type_and_name.column;
                const auto * column_array = checkAndGetColumn<ColumnArray>(column_array_ptr.get());

                const DataTypePtr & array_type_ptr = array_with_type_and_name.type;
                const auto * array_type = checkAndGetDataType<DataTypeArray>(array_type_ptr.get());

                if (!column_array)
                {
                    const ColumnConst * column_const_array = checkAndGetColumnConst<ColumnArray>(column_array_ptr.get());
                    if (!column_const_array)
                        throw Exception("X2 Expected array column, found " + column_array_ptr->getName(), ErrorCodes::ILLEGAL_COLUMN);
                    column_array_ptr = recursiveRemoveLowCardinality(column_const_array->convertToFullColumn());
                    column_array = checkAndGetColumn<ColumnArray>(column_array_ptr.get());
                }

                if (!array_type)
                    throw Exception("X3 Expected array type, found " + array_type_ptr->getName(), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

                if (!offsets_column)
                {
                    offsets_column = column_array->getOffsetsPtr();
                }
                else
                {
                    /// The first condition is optimization: do not compare data if the pointers are equal.
                    if (column_array->getOffsetsPtr() != offsets_column
                        && column_array->getOffsets() != typeid_cast<const ColumnArray::ColumnOffsets &>(*offsets_column).getData())
                        throw Exception("Arrays passed to " + getName() + " must have equal size", ErrorCodes::SIZES_OF_ARRAYS_DOESNT_MATCH);
                }

                if (i == 1)
                {
                    column_first_array_ptr = column_array_ptr;
                    column_first_array = column_array;
                }

                arrays.emplace_back(ColumnWithTypeAndName(column_array->getDataPtr(),
                                                          recursiveRemoveLowCardinality(array_type->getNestedType()),
                                                          array_with_type_and_name.name));
            }
            if (Impl::isFolding())
                arrays.emplace_back(arguments[arguments.size() - 1]); // TODO .last()
            std::cerr << "  arrays.size() = " << arrays.size() << std::endl;
            std::cerr << "  column_first_array->size() = " << column_first_array->size() << std::endl;
            std::cerr << "  column_first_array->getOffsets().size() = " << column_first_array->getOffsets().size() << std::endl;
            std::cerr << "  column_first_array->getData().size() = " << column_first_array->getData().size() << std::endl;

            if (Impl::isFolding() && (column_first_array->getData().size() > 0)) // TODO .size() -> .empty()
            {

                size_t arr_cursor = 0;

                MutableColumnPtr result = arguments.back().column->convertToFullColumnIfConst()->cloneEmpty();

                for(size_t irow = 0; irow < column_first_array->size(); ++irow) // for each row of result
                {
                    std::cerr << "  --- row " << irow << "  ---" << std::endl;

                    // Make accumulator column for this row
                    // TODO проверить с константой
                    ColumnWithTypeAndName accumulator_column = arguments.back(); // TODO тут нужно ещё и позицию в аргументе извлекать
                    ColumnPtr acc(accumulator_column.column->cut(irow, 1));

                    std::cerr << "  * accumulator.type " << accumulator_column.type->getName() << std::endl;
                    std::cerr << "  * accumulator.column " << accumulator_column.column->dumpStructure() << std::endl;
                    std::cerr << "  * acc "    << acc->dumpStructure() << std::endl;
                    std::cerr << "  * acc[0] " << (*acc)[0].dump() << std::endl;

                    auto accumulator = ColumnWithTypeAndName(acc,
                                                             accumulator_column.type,
                                                             accumulator_column.name);

                    ColumnPtr res;
                    size_t const arr_next = column_first_array->getOffsets()[irow]; // when we do folding
                    for(size_t iter = 0; arr_cursor < arr_next; ++iter, ++arr_cursor)
                    {
                        std::cerr << "  ----- iteration " << iter << "  ------" << std::endl;
                        // Make slice of input arrays and accumulator for lambda
                        ColumnsWithTypeAndName iter_arrays;
                        std::cerr << "    arrays.size() = " << arrays.size() << std::endl;
                        iter_arrays.reserve(arrays.size() + 1);
                        //size_t arr_from = (iter == 0) ? 0 : column_first_array->getOffsets()[iter - 1];
                        //size_t arr_len = column_first_array->getOffsets()[iter] - arr_from;
                        //std::cerr << "  arr_from = " << arr_from << std::endl;
                        //std::cerr << "  arr_len  = " << arr_len  << std::endl;
                        std::cerr << "    arr_cursor = " << arr_cursor << std::endl;
                        std::cerr << "    arr_next   = " << arr_next << std::endl;
                        for(size_t icolumn = 0; icolumn < arrays.size() - 1; ++icolumn)
                        {
                            auto const & arr = arrays[icolumn];
                            std::cerr << "    @ " << icolumn << ") 1 :: " << arr_cursor << std::endl;
                            /*
                            const ColumnArray * arr_array = checkAndGetColumn<ColumnArray>(arr.column.get());
                            std::cerr << "    " << icolumn << ") " << 1 << " " << arr_array << std::endl;
                            std::cerr << "    " << icolumn << ") " << 2 << std::endl;
                            const ColumnPtr & nested_column_x = arr_array->getData().cut(iter, 1);
                            std::cerr << "    " << icolumn << ") " << 3 << std::endl;
                            const ColumnPtr & offsets_column_x = ColumnArray::ColumnOffsets::create(1, 1);
                            std::cerr << "    " << icolumn << ") " << 4 << std::endl;
                            auto new_arr_array = ColumnArray::create(nested_column_x, offsets_column_x);
                            std::cerr << "    " << icolumn << ") " << 5 << std::endl;
                            */
                            iter_arrays.emplace_back(ColumnWithTypeAndName(arr.column->cut(arr_cursor, 1),
                                                                           arr.type,
                                                                           arr.name));
                            std::cerr << "    @ " << icolumn << ") 2 :: " << iter_arrays.back().column->dumpStructure() << std::endl;
                        }
                        iter_arrays.emplace_back(accumulator);
                        // ----
                        std::cerr << "    formed" << std::endl;
                        auto replicated_column_function_ptr = IColumn::mutate(column_function->replicate(column_first_array->getOffsets()));
                        auto * replicated_column_function = typeid_cast<ColumnFunction *>(replicated_column_function_ptr.get());
                        std::cerr << "    pre append" << std::endl;
                        replicated_column_function->appendArguments(iter_arrays);
                        std::cerr << "    post append" << std::endl;
                        auto lambda_result = replicated_column_function->reduce().column;
                        if (lambda_result->lowCardinality())
                            lambda_result = lambda_result->convertToFullColumnIfLowCardinality();
                        std::cerr << "    pre execute" << std::endl;
                        res = Impl::execute(*column_first_array, lambda_result); // TODO column_first_array
                        std::cerr << "  post execute : res " << res->dumpStructure() << std::endl;
                        std::cerr << "  post execute : res[0] " << (*res)[0].dump() << std::endl;
                        // ~~~
                        // ~~~
                        // ~~~
                        accumulator.column = res;

                    }

                    std::cerr << "  pre result " << result->dumpStructure() << std::endl;
                    //result->insertFrom(*res, 0);
                    result->insert((*res)[0]);
                    std::cerr << "  post result " << result->dumpStructure() << std::endl;
                    std::cerr << "  post res[0] " << (*res)[0].dump() << std::endl;
                    std::cerr << "  post result[0] " << (*result)[0].dump() << std::endl;

                    //return res;

                }
                return result;

            }
            else
            {
                /// Put all the necessary columns multiplied by the sizes of arrays into the columns.
                auto replicated_column_function_ptr = IColumn::mutate(column_function->replicate(column_first_array->getOffsets()));
                auto * replicated_column_function = typeid_cast<ColumnFunction *>(replicated_column_function_ptr.get());
                replicated_column_function->appendArguments(arrays);

                auto lambda_result = replicated_column_function->reduce().column;
                if (lambda_result->lowCardinality())
                    lambda_result = lambda_result->convertToFullColumnIfLowCardinality();

                ColumnPtr res = Impl::execute(*column_first_array, lambda_result);
                std::cerr << " ^^^ FOLDING" << std::endl;
                return res;
                //return Impl::execute(*column_first_array, lambda_result);
            }
        }
    }
};

}
