#include <Flash/Coprocessor/ArrowColCodec.h>

#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <DataTypes/DataTypeDecimal.h>
#include <DataTypes/DataTypeMyDate.h>
#include <DataTypes/DataTypeMyDateTime.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Flash/Coprocessor/DAGUtils.h>
#include <Functions/FunctionHelpers.h>
#include <IO/Endian.h>

namespace DB
{

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int UNKNOWN_EXCEPTION;
} // namespace ErrorCodes

const IColumn * getNestedCol(const IColumn * flash_col)
{
    if (flash_col->isColumnNullable())
        return dynamic_cast<const ColumnNullable *>(flash_col)->getNestedColumnPtr().get();
    else
        return flash_col;
}

template <typename T>
void decimalToVector(T dec, std::vector<Int32> & vec, UInt32 scale)
{
    Int256 value = dec.value;
    if (value < 0)
    {
        value = -value;
    }
    while (value != 0)
    {
        vec.push_back(static_cast<Int32>(value % 10));
        value = value / 10;
    }
    while (vec.size() < scale)
    {
        vec.push_back(0);
    }
}

template <typename T, bool is_nullable>
bool flashDecimalColToArrowColInternal(
    TiDBColumn & dag_column, const IColumn * flash_col_untyped, size_t start_index, size_t end_index, const IDataType * data_type)
{
    const IColumn * nested_col = getNestedCol(flash_col_untyped);
    if (checkColumn<ColumnDecimal<T>>(nested_col) && checkDataType<DataTypeDecimal<T>>(data_type))
    {
        const ColumnDecimal<T> * flash_col = checkAndGetColumn<ColumnDecimal<T>>(nested_col);
        const DataTypeDecimal<T> * type = checkAndGetDataType<DataTypeDecimal<T>>(data_type);
        for (size_t i = start_index; i < end_index; i++)
        {
            if constexpr (is_nullable)
            {
                if (flash_col_untyped->isNullAt(i))
                {
                    dag_column.appendNull();
                    continue;
                }
            }
            const T & dec = flash_col->getElement(i);
            std::vector<Int32> digits;
            UInt32 scale = type->getScale();
            decimalToVector<T>(dec, digits, scale);
            TiDBDecimal tiDecimal(scale, digits, dec.value < 0);
            dag_column.append(tiDecimal);
        }
        return true;
    }
    return false;
}

template <bool is_nullable>
void flashDecimalColToArrowCol(
    TiDBColumn & dag_column, const IColumn * flash_col_untyped, size_t start_index, size_t end_index, const IDataType * data_type)
{
    if (!(flashDecimalColToArrowColInternal<Decimal32, is_nullable>(dag_column, flash_col_untyped, start_index, end_index, data_type)
            || flashDecimalColToArrowColInternal<Decimal64, is_nullable>(dag_column, flash_col_untyped, start_index, end_index, data_type)
            || flashDecimalColToArrowColInternal<Decimal128, is_nullable>(dag_column, flash_col_untyped, start_index, end_index, data_type)
            || flashDecimalColToArrowColInternal<Decimal256, is_nullable>(
                dag_column, flash_col_untyped, start_index, end_index, data_type)))
        throw Exception("Error while trying to convert flash col to DAG col, "
                        "column name "
                + flash_col_untyped->getName(),
            ErrorCodes::UNKNOWN_EXCEPTION);
}

template <typename T, bool is_nullable>
bool flashIntegerColToArrowColInternal(TiDBColumn & dag_column, const IColumn * flash_col_untyped, size_t start_index, size_t end_index)
{
    const IColumn * nested_col = getNestedCol(flash_col_untyped);
    if (const ColumnVector<T> * flash_col = checkAndGetColumn<ColumnVector<T>>(nested_col))
    {
        constexpr bool is_unsigned = std::is_unsigned_v<T>;
        for (size_t i = start_index; i < end_index; i++)
        {
            if constexpr (is_nullable)
            {
                if (flash_col_untyped->isNullAt(i))
                {
                    dag_column.appendNull();
                    continue;
                }
            }
            if constexpr (is_unsigned)
                dag_column.append((UInt64)flash_col->getElement(i));
            else
                dag_column.append((Int64)flash_col->getElement(i));
        }
        return true;
    }
    return false;
}

template <typename T, bool is_nullable>
void flashDoubleColToArrowCol(TiDBColumn & dag_column, const IColumn * flash_col_untyped, size_t start_index, size_t end_index)
{
    const IColumn * nested_col = getNestedCol(flash_col_untyped);
    if (const ColumnVector<T> * flash_col = checkAndGetColumn<ColumnVector<T>>(nested_col))
    {
        for (size_t i = start_index; i < end_index; i++)
        {
            if constexpr (is_nullable)
            {
                if (flash_col_untyped->isNullAt(i))
                {
                    dag_column.appendNull();
                    continue;
                }
            }
            dag_column.append((T)flash_col->getElement(i));
        }
        return;
    }
    throw Exception("Error while trying to convert flash col to DAG col, "
                    "column name "
            + flash_col_untyped->getName(),
        ErrorCodes::UNKNOWN_EXCEPTION);
}

template <bool is_nullable>
void flashIntegerColToArrowCol(TiDBColumn & dag_column, const IColumn * flash_col_untyped, size_t start_index, size_t end_index)
{
    if (!(flashIntegerColToArrowColInternal<UInt8, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)
            || flashIntegerColToArrowColInternal<UInt16, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)
            || flashIntegerColToArrowColInternal<UInt32, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)
            || flashIntegerColToArrowColInternal<UInt64, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)
            || flashIntegerColToArrowColInternal<Int8, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)
            || flashIntegerColToArrowColInternal<Int16, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)
            || flashIntegerColToArrowColInternal<Int32, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)
            || flashIntegerColToArrowColInternal<Int64, is_nullable>(dag_column, flash_col_untyped, start_index, end_index)))
        throw Exception("Error while trying to convert flash col to DAG col, "
                        "column name "
                + flash_col_untyped->getName(),
            ErrorCodes::UNKNOWN_EXCEPTION);
}


template <bool is_nullable>
void flashDateOrDateTimeColToArrowCol(
    TiDBColumn & dag_column, const IColumn * flash_col_untyped, size_t start_index, size_t end_index, const tipb::FieldType & field_type)
{
    const IColumn * nested_col = getNestedCol(flash_col_untyped);
    using DateFieldType = DataTypeMyTimeBase::FieldType;
    auto * flash_col = checkAndGetColumn<ColumnVector<DateFieldType>>(nested_col);
    for (size_t i = start_index; i < end_index; i++)
    {
        if constexpr (is_nullable)
        {
            if (flash_col_untyped->isNullAt(i))
            {
                dag_column.appendNull();
                continue;
            }
        }
        TiDBTime time = TiDBTime(flash_col->getElement(i), field_type);
        dag_column.append(time);
    }
}

template <bool is_nullable>
void flashStringColToArrowCol(TiDBColumn & dag_column, const IColumn * flash_col_untyped, size_t start_index, size_t end_index)
{
    const IColumn * nested_col = getNestedCol(flash_col_untyped);
    // columnFixedString is not used so do not check it
    auto * flash_col = checkAndGetColumn<ColumnString>(nested_col);
    for (size_t i = start_index; i < end_index; i++)
    {
        // todo check if we can convert flash_col to DAG col directly since the internal representation is almost the same
        if constexpr (is_nullable)
        {
            if (flash_col_untyped->isNullAt(i))
            {
                dag_column.appendNull();
                continue;
            }
        }
        dag_column.append(flash_col->getDataAt(i));
    }
}

void flashColToArrowCol(TiDBColumn & dag_column, const ColumnWithTypeAndName & flash_col, const tipb::FieldType & field_type,
    size_t start_index, size_t end_index)
{
    const IColumn * col = flash_col.column.get();
    const IDataType * type = flash_col.type.get();
    const TiDB::ColumnInfo tidb_column_info = fieldTypeToColumnInfo(field_type);

    if (type->isNullable() && tidb_column_info.hasNotNullFlag())
    {
        throw Exception("Flash column and TiDB column has different not null flag", ErrorCodes::LOGICAL_ERROR);
    }
    if (type->isNullable())
        type = dynamic_cast<const DataTypeNullable *>(type)->getNestedType().get();

    switch (tidb_column_info.tp)
    {
        case TiDB::TypeTiny:
        case TiDB::TypeShort:
        case TiDB::TypeInt24:
        case TiDB::TypeLong:
        case TiDB::TypeLongLong:
        case TiDB::TypeYear:
            if (!type->isInteger())
                throw Exception("Type un-matched during arrow encode, target col type is integer and source column"
                                " type is "
                        + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
            if (type->isUnsignedInteger() != tidb_column_info.hasUnsignedFlag())
                throw Exception("Flash column and TiDB column has different unsigned flag", ErrorCodes::LOGICAL_ERROR);
            if (tidb_column_info.hasNotNullFlag())
                flashIntegerColToArrowCol<false>(dag_column, col, start_index, end_index);
            else
                flashIntegerColToArrowCol<true>(dag_column, col, start_index, end_index);
            break;
        case TiDB::TypeFloat:
            if (!checkDataType<DataTypeFloat32>(type))
                throw Exception("Type un-matched during arrow encode, target col type is float32 and source column"
                                " type is "
                        + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
            if (tidb_column_info.hasNotNullFlag())
                flashDoubleColToArrowCol<Float32, false>(dag_column, col, start_index, end_index);
            else
                flashDoubleColToArrowCol<Float32, true>(dag_column, col, start_index, end_index);
            break;
        case TiDB::TypeDouble:
            if (!checkDataType<DataTypeFloat64>(type))
                throw Exception("Type un-matched during arrow encode, target col type is float64 and source column"
                                " type is "
                        + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
            if (tidb_column_info.hasNotNullFlag())
                flashDoubleColToArrowCol<Float64, false>(dag_column, col, start_index, end_index);
            else
                flashDoubleColToArrowCol<Float64, true>(dag_column, col, start_index, end_index);
            break;
        case TiDB::TypeDate:
        case TiDB::TypeDatetime:
        case TiDB::TypeTimestamp:
            if (!type->isDateOrDateTime())
                throw Exception("Type un-matched during arrow encode, target col type is datetime and source column"
                                " type is "
                        + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
            if (tidb_column_info.hasNotNullFlag())
                flashDateOrDateTimeColToArrowCol<false>(dag_column, col, start_index, end_index, field_type);
            else
                flashDateOrDateTimeColToArrowCol<true>(dag_column, col, start_index, end_index, field_type);
            break;
        case TiDB::TypeNewDecimal:
            if (!type->isDecimal())
                throw Exception("Type un-matched during arrow encode, target col type is datetime and source column"
                                " type is "
                        + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
            if (tidb_column_info.hasNotNullFlag())
                flashDecimalColToArrowCol<false>(dag_column, col, start_index, end_index, type);
            else
                flashDecimalColToArrowCol<true>(dag_column, col, start_index, end_index, type);
            break;
        case TiDB::TypeVarchar:
        case TiDB::TypeVarString:
        case TiDB::TypeString:
        case TiDB::TypeBlob:
        case TiDB::TypeLongBlob:
        case TiDB::TypeMediumBlob:
        case TiDB::TypeTinyBlob:
            if (!checkDataType<DataTypeString>(type))
                throw Exception("Type un-matched during arrow encode, target col type is string and source column"
                                " type is "
                        + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
            if (tidb_column_info.hasNotNullFlag())
                flashStringColToArrowCol<false>(dag_column, col, start_index, end_index);
            else
                flashStringColToArrowCol<true>(dag_column, col, start_index, end_index);
            break;
        default:
            throw Exception("Unsupported field type " + field_type.DebugString() + " when try to convert flash col to DAG col",
                ErrorCodes::NOT_IMPLEMENTED);
    }
}

bool checkNull(UInt32 i, UInt32 null_count, const std::vector<UInt8> & null_bitmap, const ColumnWithTypeAndName & col)
{
    if (null_count > 0)
    {
        size_t index = i >> 3;
        size_t p = i & 7;
        if (!(null_bitmap[index] & (1 << p)))
        {
            col.column->assumeMutable()->insert(Field());
            return true;
        }
    }
    return false;
}

const char * arrowStringColToFlashCol(const char * pos, UInt8, UInt32 null_count, const std::vector<UInt8> & null_bitmap,
    const std::vector<UInt64> & offsets, const ColumnWithTypeAndName & col, const ColumnInfo &, UInt32 length)
{
    for (UInt32 i = 0; i < length; i++)
    {
        if (checkNull(i, null_count, null_bitmap, col))
            continue;
        const String value = String(pos + offsets[i], pos + offsets[i + 1]);
        col.column->assumeMutable()->insert(Field(value));
    }
    return pos + offsets[length];
}

template <typename T>
T toCHDecimal(UInt8 digits_int, UInt8 digits_frac, bool negative, const Int32 * word_buf)
{
    static_assert(IsDecimal<T>);

    UInt8 word_int = (digits_int + DIGITS_PER_WORD - 1) / DIGITS_PER_WORD;
    UInt8 word_frac = digits_frac / DIGITS_PER_WORD;
    UInt8 tailing_digit = digits_frac % DIGITS_PER_WORD;

    typename T::NativeType value = 0;
    const int word_max = int(1e9);
    for (int i = 0; i < word_int; i++)
    {
        value = value * word_max + word_buf[i];
    }
    for (int i = 0; i < word_frac; i++)
    {
        value = value * word_max + word_buf[i + word_int];
    }
    if (tailing_digit > 0)
    {
        Int32 tail = word_buf[word_int + word_frac];
        for (int i = 0; i < DIGITS_PER_WORD - tailing_digit; i++)
        {
            tail /= 10;
        }
        for (int i = 0; i < tailing_digit; i++)
        {
            value *= 10;
        }
        value += tail;
    }
    return negative ? -value : value;
}

const char * arrowDecimalColToFlashCol(const char * pos, UInt8 field_length, UInt32 null_count, const std::vector<UInt8> & null_bitmap,
    const std::vector<UInt64> &, const ColumnWithTypeAndName & col, const ColumnInfo &, UInt32 length)
{
    for (UInt32 i = 0; i < length; i++)
    {
        if (checkNull(i, null_count, null_bitmap, col))
        {
            pos += field_length;
            continue;
        }
        UInt8 digits_int = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        UInt8 digits_frac = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        //UInt8 result_frac = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        UInt8 negative = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        Int32 word_buf[MAX_WORD_BUF_LEN];
        const DataTypePtr decimal_type
            = col.type->isNullable() ? dynamic_cast<const DataTypeNullable *>(col.type.get())->getNestedType() : col.type;
        for (int j = 0; j < MAX_WORD_BUF_LEN; j++)
        {
            word_buf[j] = toLittleEndian(*(reinterpret_cast<const Int32 *>(pos)));
            pos += 4;
        }
        if (auto * type32 = checkDecimal<Decimal32>(*decimal_type))
        {
            auto res = toCHDecimal<Decimal32>(digits_int, digits_frac, negative, word_buf);
            col.column->assumeMutable()->insert(DecimalField<Decimal32>(res, type32->getScale()));
        }
        else if (auto * type64 = checkDecimal<Decimal64>(*decimal_type))
        {
            auto res = toCHDecimal<Decimal64>(digits_int, digits_frac, negative, word_buf);
            col.column->assumeMutable()->insert(DecimalField<Decimal64>(res, type64->getScale()));
        }
        else if (auto * type128 = checkDecimal<Decimal128>(*decimal_type))
        {
            auto res = toCHDecimal<Decimal128>(digits_int, digits_frac, negative, word_buf);
            col.column->assumeMutable()->insert(DecimalField<Decimal128>(res, type128->getScale()));
        }
        else if (auto * type256 = checkDecimal<Decimal256>(*decimal_type))
        {
            auto res = toCHDecimal<Decimal256>(digits_int, digits_frac, negative, word_buf);
            col.column->assumeMutable()->insert(DecimalField<Decimal256>(res, type256->getScale()));
        }
    }
    return pos;
}

const char * arrowDateColToFlashCol(const char * pos, UInt8 field_length, UInt32 null_count, const std::vector<UInt8> & null_bitmap,
    const std::vector<UInt64> &, const ColumnWithTypeAndName & col, const ColumnInfo &, UInt32 length)
{
    for (UInt32 i = 0; i < length; i++)
    {
        if (checkNull(i, null_count, null_bitmap, col))
        {
            pos += field_length;
            continue;
        }
        UInt32 hour = toLittleEndian(*(reinterpret_cast<const UInt32 *>(pos)));
        pos += 4;
        UInt32 micro_second = toLittleEndian(*(reinterpret_cast<const UInt32 *>(pos)));
        pos += 4;
        UInt16 year = toLittleEndian(*(reinterpret_cast<const UInt16 *>(pos)));
        pos += 2;
        UInt8 month = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        UInt8 day = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        UInt8 minute = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        UInt8 second = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        pos += 2;
        //UInt8 time_type = toLittleEndian(*(reinterpret_cast<const UInt8 *>(pos)));
        pos += 1;
        //UInt8 fsp = toLittleEndian(*(reinterpret_cast<const Int8 *>(pos)));
        pos += 1;
        pos += 2;
        MyDateTime mt(year, month, day, hour, minute, second, micro_second);
        col.column->assumeMutable()->insert(Field(mt.toPackedUInt()));
    }
    return pos;
}

const char * arrowNumColToFlashCol(const char * pos, UInt8 field_length, UInt32 null_count, const std::vector<UInt8> & null_bitmap,
    const std::vector<UInt64> &, const ColumnWithTypeAndName & col, const ColumnInfo & col_info, UInt32 length)
{
    for (UInt32 i = 0; i < length; i++, pos += field_length)
    {
        if (checkNull(i, null_count, null_bitmap, col))
            continue;
        UInt64 u64;
        Int64 i64;
        UInt32 u32;
        Float32 f32;
        Float64 f64;
        switch (col_info.tp)
        {
            case TiDB::TypeTiny:
            case TiDB::TypeShort:
            case TiDB::TypeInt24:
            case TiDB::TypeLong:
            case TiDB::TypeLongLong:
            case TiDB::TypeYear:
                if (col_info.flag & TiDB::ColumnFlagUnsigned)
                {
                    u64 = toLittleEndian(*(reinterpret_cast<const UInt64 *>(pos)));
                    col.column->assumeMutable()->insert(Field(u64));
                }
                else
                {
                    i64 = toLittleEndian(*(reinterpret_cast<const Int64 *>(pos)));
                    col.column->assumeMutable()->insert(Field(i64));
                }
                break;
            case TiDB::TypeFloat:
                u32 = toLittleEndian(*(reinterpret_cast<const UInt32 *>(pos)));
                std::memcpy(&f32, &u32, sizeof(Float32));
                col.column->assumeMutable()->insert(Field((Float64)f32));
                break;
            case TiDB::TypeDouble:
                u64 = toLittleEndian(*(reinterpret_cast<const UInt64 *>(pos)));
                std::memcpy(&f64, &u64, sizeof(Float64));
                col.column->assumeMutable()->insert(Field(f64));
                break;
            default:
                throw Exception("Should not reach here", ErrorCodes::LOGICAL_ERROR);
        }
    }
    return pos;
}

const char * arrowColToFlashCol(const char * pos, UInt8 field_length, UInt32 null_count, const std::vector<UInt8> & null_bitmap,
    const std::vector<UInt64> & offsets, const ColumnWithTypeAndName & flash_col, const ColumnInfo & col_info, UInt32 length)
{
    switch (col_info.tp)
    {
        case TiDB::TypeTiny:
        case TiDB::TypeShort:
        case TiDB::TypeInt24:
        case TiDB::TypeLong:
        case TiDB::TypeLongLong:
        case TiDB::TypeYear:
        case TiDB::TypeFloat:
        case TiDB::TypeDouble:
            return arrowNumColToFlashCol(pos, field_length, null_count, null_bitmap, offsets, flash_col, col_info, length);
        case TiDB::TypeDatetime:
        case TiDB::TypeDate:
        case TiDB::TypeTimestamp:
            return arrowDateColToFlashCol(pos, field_length, null_count, null_bitmap, offsets, flash_col, col_info, length);
        case TiDB::TypeNewDecimal:
            return arrowDecimalColToFlashCol(pos, field_length, null_count, null_bitmap, offsets, flash_col, col_info, length);
        case TiDB::TypeVarString:
        case TiDB::TypeVarchar:
        case TiDB::TypeBlob:
        case TiDB::TypeString:
        case TiDB::TypeTinyBlob:
        case TiDB::TypeMediumBlob:
        case TiDB::TypeLongBlob:
            return arrowStringColToFlashCol(pos, field_length, null_count, null_bitmap, offsets, flash_col, col_info, length);
        default:
            throw Exception("Not supported yet: field tp = " + std::to_string(col_info.tp));
    }
}

} // namespace DB