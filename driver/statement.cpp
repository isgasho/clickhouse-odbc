#include "platform.h"
#include "utils.h"
#include "statement.h"
#include "escaping/lexer.h"
#include "escaping/escape_sequences.h"

#include <Poco/Exception.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/URI.h>
#include <Poco/UUID.h>
#include <Poco/UUIDGenerator.h>

namespace {

    template <typename T>
    struct to {
        template <typename F>
        static T from(const BindingInfo& binding_info) {
            throw std::runtime_error("Unable to extract data from bound buffer: conversion from source type to target type not supported");
        }
    };

    template <>
    struct to<std::string> {
        template <typename F>
        static std::string from(const BindingInfo& binding_info) {
            if (!binding_info.value)
                return std::string{};

            const auto * ind_ptr = binding_info.indicator;

            if (ind_ptr) {
                switch (*ind_ptr) {
                    case 0:
                    case SQL_NTS:
                        break;

                    case SQL_NULL_DATA:
                        return std::string{};

                    case SQL_DEFAULT_PARAM:
                        return std::to_string(F{});

                    default:
                        if (*ind_ptr == SQL_DATA_AT_EXEC || *ind_ptr < 0)
                            throw std::runtime_error("Unable to extract data from bound buffer: data-at-execution bindings not supported");
                }
            }

            return std::to_string(*(F*)binding_info.value);
        }

        template <>
        std::string from<SQLCHAR *>(const BindingInfo& binding_info) {
            const auto * cstr = reinterpret_cast<const char *>(binding_info.value);

            if (!cstr)
                return std::string{};

            const auto * sz_ptr = binding_info.value_size;
            const auto * ind_ptr = binding_info.indicator;

            if (ind_ptr) {
                switch (*ind_ptr) {
                    case 0:
                    case SQL_NTS:
                        return std::string{cstr};

                    case SQL_NULL_DATA:
                    case SQL_DEFAULT_PARAM:
                        return std::string{};

                    default:
                        if (*ind_ptr == SQL_DATA_AT_EXEC || *ind_ptr < 0)
                            throw std::runtime_error("Unable to extract data from bound buffer: data-at-execution bindings not supported");
                }
            }

            if (!sz_ptr || *sz_ptr < 0)
                std::string{cstr};

            return std::string{cstr, static_cast<std::size_t>(*sz_ptr)};
        }

        template <>
        std::string from<SQLWCHAR *>(const BindingInfo& binding_info) {
            const auto * wcstr = reinterpret_cast<const wchar_t *>(binding_info.value);

            if (!wcstr)
                return std::string{};

            const auto * sz_ptr = binding_info.value_size;
            const auto * ind_ptr = binding_info.indicator;

            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> convert;

            if (ind_ptr) {
                switch (*ind_ptr) {
                    case 0:
                    case SQL_NTS:
                        return convert.to_bytes(wcstr);

                    case SQL_NULL_DATA:
                    case SQL_DEFAULT_PARAM:
                        return std::string{};

                    default:
                        if (*ind_ptr == SQL_DATA_AT_EXEC || *ind_ptr < 0)
                            throw std::runtime_error("Unable to extract data from bound buffer: data-at-execution bindings not supported");
                }
            }

            if (!sz_ptr || *sz_ptr < 0)
                convert.to_bytes(wcstr);

            const auto* wcstr_last = wcstr + static_cast<std::size_t>(*sz_ptr) / sizeof(decltype(*wcstr));
            return convert.to_bytes(wcstr, wcstr_last);
        }
    };

    template <typename T>
    T read_ready_data_to(const BindingInfo& binding_info) {
        switch (binding_info.type) {
            case SQL_C_CHAR:        return to<T>::template from<SQLCHAR *    >(binding_info);
            case SQL_C_WCHAR:       return to<T>::template from<SQLWCHAR *   >(binding_info);
            case SQL_C_SSHORT:      return to<T>::template from<SQLSMALLINT  >(binding_info);
            case SQL_C_USHORT:      return to<T>::template from<SQLUSMALLINT >(binding_info);
            case SQL_C_SLONG:       return to<T>::template from<SQLINTEGER   >(binding_info);
            case SQL_C_ULONG:       return to<T>::template from<SQLUINTEGER  >(binding_info);
            case SQL_C_FLOAT:       return to<T>::template from<SQLREAL      >(binding_info);
            case SQL_C_DOUBLE:      return to<T>::template from<SQLDOUBLE    >(binding_info);
            case SQL_C_BIT:         return to<T>::template from<SQLCHAR      >(binding_info);
            case SQL_C_STINYINT:    return to<T>::template from<SQLSCHAR     >(binding_info);
            case SQL_C_UTINYINT:    return to<T>::template from<SQLCHAR      >(binding_info);
            case SQL_C_SBIGINT:     return to<T>::template from<SQLBIGINT    >(binding_info);
            case SQL_C_UBIGINT:     return to<T>::template from<SQLUBIGINT   >(binding_info);
            case SQL_C_BINARY:      return to<T>::template from<SQLCHAR *    >(binding_info);
//          case SQL_C_BOOKMARK:    return to<T>::template from<BOOKMARK     >(binding_info);
//          case SQL_C_VARBOOKMARK: return to<T>::template from<SQLCHAR *    >(binding_info);

            default:
                throw std::runtime_error("Unable to extract data from bound buffer: source type representation not supported");
        }
    }

} // namespace

Statement::Statement(Connection & connection)
    : ChildType(connection)
{
    allocate_implicit_descriptors();
}

Statement::~Statement() {
    deallocate_implicit_descriptors();
}

const TypeInfo & Statement::getTypeInfo(const std::string & type_name, const std::string & type_name_without_parametrs) const {
    return get_parent().get_parent().getTypeInfo(type_name, type_name_without_parametrs);
}

void Statement::prepareQuery(const std::string & q) {
    close_cursor();
    query = q;
    processEscapeSequences();
    extractParametersinfo();
}

void Statement::executeQuery(IResultMutatorPtr && mutator) {
    auto * param_set_processed_ptr = get_effective_descriptor(SQL_ATTR_IMP_PARAM_DESC).get_attr_as<SQLULEN *>(SQL_DESC_ROWS_PROCESSED_PTR, 0);
    if (param_set_processed_ptr)
        *param_set_processed_ptr = 0;

    next_param_set = 0;
    requestNextPackOfResultSets(std::move(mutator));
}

void Statement::requestNextPackOfResultSets(IResultMutatorPtr && mutator) {
    const auto param_set_array_size = get_effective_descriptor(SQL_ATTR_APP_PARAM_DESC).get_attr_as<SQLULEN>(SQL_DESC_ARRAY_SIZE, 1);
    if (next_param_set >= param_set_array_size)
        return;

    get_diag_header().set_attr(SQL_DIAG_ROW_COUNT, 0);

    auto & connection = get_parent();

    if (connection.session && response && in)
        if (!*in || in->peek() != EOF)
            connection.session->reset();

    Poco::URI uri(connection.url);
    uri.addQueryParameter("database", connection.getDatabase());
    uri.addQueryParameter("default_format", "ODBCDriver2");

    const auto param_bindings = getParamsBindingInfo();

    if (parameters.size() < param_bindings.size())
        throw SqlException("COUNT field incorrect", "07002");

    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto & param_info = parameters[i];
        const auto & binding_info = param_bindings[i];

        if (!is_input_param(binding_info.io_type) || is_stream_param(binding_info.io_type))
            throw std::runtime_error("Unable to extract data from bound param buffer: param IO type is not supported");

        uri.addQueryParameter("param_" + param_info.name, read_ready_data_to<std::string>(binding_info));
    }

    const auto prepared_query = buildFinalQuery(param_bindings);

    // TODO: set this only after this single query is fully fetched (when output parameter support is added)
    auto * param_set_processed_ptr = get_effective_descriptor(SQL_ATTR_IMP_PARAM_DESC).get_attr_as<SQLULEN *>(SQL_DESC_ROWS_PROCESSED_PTR, 0);
    if (param_set_processed_ptr)
        *param_set_processed_ptr = next_param_set;

    Poco::Net::HTTPRequest request;
    request.setMethod(Poco::Net::HTTPRequest::HTTP_POST);
    request.setVersion(Poco::Net::HTTPRequest::HTTP_1_1);
    request.setKeepAlive(true);
    request.setChunkedTransferEncoding(true);
    request.setCredentials("Basic", connection.buildCredentialsString());
    request.setURI(uri.toString());
    request.set("User-Agent", connection.buildUserAgentString());

    LOG(request.getMethod() << " " << connection.session->getHost() << request.getURI() << " body=" << prepared_query
                            << " UA=" << request.get("User-Agent"));

    // LOG("curl 'http://" << connection.session->getHost() << ":" << connection.session->getPort() << request.getURI() << "' -d '" << prepared_query << "'");

    // Send request to server with finite count of retries.
    for (int i = 1;; ++i) {
        try {
            connection.session->sendRequest(request) << prepared_query;
            response = std::make_unique<Poco::Net::HTTPResponse>();
            in = &connection.session->receiveResponse(*response);
            break;
        } catch (const Poco::IOException & e) {
            connection.session->reset(); // reset keepalived connection
            LOG("Http request try=" << i << "/" << connection.retry_count << " failed: " << e.what() << ": " << e.message());
            if (i > connection.retry_count)
                throw;
        }
    }

    Poco::Net::HTTPResponse::HTTPStatus status = response->getStatus();
    if (status != Poco::Net::HTTPResponse::HTTP_OK) {
        std::stringstream error_message;
        error_message << "HTTP status code: " << status << std::endl << "Received error:" << std::endl << in->rdbuf() << std::endl;
        LOG(error_message.str());
        throw std::runtime_error(error_message.str());
    }

    result_set.reset(new ResultSet{*in, std::move(mutator)});

    ++next_param_set;
}

void Statement::processEscapeSequences() {
    if (get_attr_as<SQLULEN>(SQL_ATTR_NOSCAN, SQL_NOSCAN_OFF) != SQL_NOSCAN_ON)
        query = replaceEscapeSequences(query);
}

void Statement::extractParametersinfo() {
    // TODO: implement this all in an upgraded Lexer.

    parameters.clear();
    std::string placeholder;

    // Craft a temporary unique placeholder for the parameters (same for all).
    {
        Poco::UUIDGenerator uuid_gen;
        do {
            const auto uuid = uuid_gen.createOne();
            placeholder = '@' + uuid.toString();
        } while (query.find(placeholder) != std::string::npos);
    }

    // Replace all unquoted ? characters with the placeholder and populate 'parameters' array.

    char quoted_by = '\0';
    for (std::size_t i = 0; i < query.size(); ++i) {
        const char curr = query[i];
        const char next = (i < query.size() ? query[i + 1] : '\0');

        switch (curr) {
            case '\\': {
                ++i; // Skip the next char unconditionally.
                break;
            }

            case '"':
            case '\'': {
                if (quoted_by == curr) {
                    if (next == curr) {
                        ++i; // Skip the next char unconditionally: '' or "" SQL escaping.
                        break;
                    }
                    else {
                        quoted_by = '\0';
                    }
                }
                else {
                    quoted_by = curr;
                }
                break;
            }

            case '?': {
                if (quoted_by == '\0') {
                    query.replace(i, 1, placeholder);
                    i += placeholder.size();
                    i -= 1;

                    ParamInfo param_info;
                    param_info.name = "odbc_" + std::to_string(parameters.size() + 1);
                    param_info.tmp_placeholder = placeholder;
                    parameters.emplace_back(param_info);
                }
                break;
            }
        }
    }
}

std::string Statement::buildFinalQuery(const std::vector<ParamBindingInfo>& param_bindings) const {
    auto prepared_query = query;

    if (parameters.size() < param_bindings.size())
        throw SqlException("COUNT field incorrect", "07002");

    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto & param_info = parameters[i];
        const auto & binding_info = param_bindings[i];

        const auto pos = prepared_query.find(param_info.tmp_placeholder);

        if (pos == std::string::npos)
            throw SqlException("COUNT field incorrect", "07002");

        const std::string param_placeholder = "{" + param_info.name + ":" +
            convert_C_or_sql_type_to_data_source_type(binding_info.sql_type, binding_info.value_max_size) + "}";

        prepared_query.replace(pos, param_info.tmp_placeholder.size(), param_placeholder);
    }

    return prepared_query;
}

void Statement::executeQuery(const std::string & q, IResultMutatorPtr && mutator) {
    prepareQuery(q);
    executeQuery(std::move(mutator));
}

bool Statement::has_result_set() const {
    return !!result_set;
}

bool Statement::advance_to_next_result_set() {
    get_diag_header().set_attr(SQL_DIAG_ROW_COUNT, 0);

    IResultMutatorPtr mutator;

    if (has_result_set())
        mutator = result_set->release_mutator();

    // TODO: add support of detecting next result set on the wire, when protocol allows it.
    result_set.reset();

    requestNextPackOfResultSets(std::move(mutator));
    return has_result_set();
}

const ColumnInfo & Statement::getColumnInfo(size_t i) const {
    return result_set->getColumnInfo(i);
}

size_t Statement::getNumColumns() const {
    return (has_result_set() ? result_set->getNumColumns() : 0);
}

bool Statement::has_current_row() const {
    return (has_result_set() ? result_set->has_current_row() : false);
}

const Row & Statement::get_current_row() const {
    return result_set->get_current_row();
}

std::size_t Statement::get_current_row_num() const {
    return (has_result_set() ? result_set->get_current_row_num() : 0);
}

bool Statement::advance_to_next_row() {
    bool advanced = false;

    if (has_result_set()) {
        advanced = result_set->advance_to_next_row();
        if (!advanced)
            get_diag_header().set_attr(SQL_DIAG_ROW_COUNT, result_set->get_current_row_num());
    }

    return advanced;
}

void Statement::close_cursor() {
    auto & connection = get_parent();
    if (connection.session && response && in)
        if (!*in || in->peek() != EOF)
            connection.session->reset();

    result_set.reset();
    in = nullptr;
    response.reset();

    parameters.clear();
    query.clear();
}

void Statement::reset_col_bindings() {
    bindings.clear();
//  get_effective_descriptor(SQL_ATTR_APP_ROW_DESC).set_attr(SQL_DESC_COUNT, 0);
}

void Statement::reset_param_bindings() {
    get_effective_descriptor(SQL_ATTR_APP_PARAM_DESC).set_attr(SQL_DESC_COUNT, 0);
}

std::vector<ParamBindingInfo> Statement::getParamsBindingInfo() {
    std::vector<ParamBindingInfo> param_bindings;

    auto & apd_desc = get_effective_descriptor(SQL_ATTR_APP_PARAM_DESC);
    auto & ipd_desc = get_effective_descriptor(SQL_ATTR_IMP_PARAM_DESC);

    const auto apd_record_count = apd_desc.get_record_count();
    const auto ipd_record_count = ipd_desc.get_record_count();

    if (apd_record_count > ipd_record_count)
        throw SqlException("COUNT field incorrect", "07002");

    if (apd_record_count > 0)
        param_bindings.reserve(apd_record_count);

    const auto single_set_struct_size = apd_desc.get_attr_as<SQLULEN>(SQL_DESC_BIND_TYPE, SQL_PARAM_BIND_BY_COLUMN);
    const auto * bind_offset_ptr = apd_desc.get_attr_as<SQLULEN *>(SQL_DESC_BIND_OFFSET_PTR, 0);
    const auto bind_offset = (bind_offset_ptr ? *bind_offset_ptr : 0);

    param_bindings.reserve(apd_record_count);

    for (std::size_t i = 0; i < apd_record_count; ++i) {
        ParamBindingInfo binding_info;

        auto & apd_record = apd_desc.get_record(i, SQL_ATTR_APP_PARAM_DESC);
        auto & ipd_record = ipd_desc.get_record(i, SQL_ATTR_IMP_PARAM_DESC);

        const auto * data_ptr = apd_record.get_attr_as<SQLPOINTER>(SQL_DESC_DATA_PTR, 0);
        const auto * sz_ptr = apd_record.get_attr_as<SQLLEN *>(SQL_DESC_OCTET_LENGTH_PTR, 0);
        const auto * ind_ptr = apd_record.get_attr_as<SQLLEN *>(SQL_DESC_INDICATOR_PTR, 0);

        binding_info.io_type = ipd_record.get_attr_as<SQLSMALLINT>(SQL_DESC_PARAMETER_TYPE, SQL_PARAM_INPUT);
        binding_info.type = apd_record.get_attr_as<SQLSMALLINT>(SQL_DESC_CONCISE_TYPE, SQL_C_DEFAULT);
        binding_info.sql_type = ipd_record.get_attr_as<SQLSMALLINT>(SQL_DESC_CONCISE_TYPE, SQL_UNKNOWN_TYPE);
        binding_info.value_max_size = ipd_record.get_attr_as<SQLULEN>(SQL_DESC_LENGTH, 0); // TODO: or SQL_DESC_OCTET_LENGTH ?
        binding_info.value = (void *)(data_ptr ? ((char *)(data_ptr) + i * single_set_struct_size + bind_offset) : 0);
        binding_info.value_size = (SQLLEN *)(sz_ptr ? ((char *)(sz_ptr) + i * sizeof(SQLLEN) + bind_offset) : 0);
        binding_info.indicator = (SQLLEN *)(ind_ptr ? ((char *)(ind_ptr) + i * sizeof(SQLLEN) + bind_offset) : 0);

        param_bindings.emplace_back(binding_info);
    }

    return param_bindings;
}

Descriptor& Statement::get_effective_descriptor(SQLINTEGER type) {
    switch (type) {
        case SQL_ATTR_APP_ROW_DESC:   return choose(implicit_ard, explicit_ard);
        case SQL_ATTR_APP_PARAM_DESC: return choose(implicit_apd, explicit_apd);
        case SQL_ATTR_IMP_ROW_DESC:   return choose(implicit_ird, explicit_ird);
        case SQL_ATTR_IMP_PARAM_DESC: return choose(implicit_ipd, explicit_ipd);
    }
    throw std::runtime_error("unknown descriptor type");
}

void Statement::set_explicit_descriptor(SQLINTEGER type, std::shared_ptr<Descriptor> desc) {
    switch (type) {
        case SQL_ATTR_APP_ROW_DESC:   explicit_ard = desc; return;
        case SQL_ATTR_APP_PARAM_DESC: explicit_apd = desc; return;
        case SQL_ATTR_IMP_ROW_DESC:   explicit_ird = desc; return;
        case SQL_ATTR_IMP_PARAM_DESC: explicit_ipd = desc; return;
    }
    throw std::runtime_error("unknown descriptor type");
}

void Statement::set_implicit_descriptor(SQLINTEGER type) {
    return set_explicit_descriptor(type, std::shared_ptr<Descriptor>{});
}

Descriptor & Statement::choose(
    std::shared_ptr<Descriptor> & implicit_desc,
    std::weak_ptr<Descriptor> & explicit_desc
) {
    auto desc = explicit_desc.lock();
    return (desc ? *desc : *implicit_desc);
}

void Statement::allocate_implicit_descriptors() {
    deallocate_implicit_descriptors();

    implicit_ard = allocate_descriptor();
    implicit_apd = allocate_descriptor();
    implicit_ird = allocate_descriptor();
    implicit_ipd = allocate_descriptor();

    get_parent().init_as_desc(*implicit_ard, SQL_ATTR_APP_ROW_DESC);
    get_parent().init_as_desc(*implicit_apd, SQL_ATTR_APP_PARAM_DESC);
    get_parent().init_as_desc(*implicit_ird, SQL_ATTR_IMP_ROW_DESC);
    get_parent().init_as_desc(*implicit_ipd, SQL_ATTR_IMP_PARAM_DESC);
}

void Statement::deallocate_implicit_descriptors() {
    dellocate_descriptor(implicit_ard);
    dellocate_descriptor(implicit_apd);
    dellocate_descriptor(implicit_ird);
    dellocate_descriptor(implicit_ipd);
}

std::shared_ptr<Descriptor> Statement::allocate_descriptor() {
    auto & desc = get_parent().allocate_child<Descriptor>();
    return desc.shared_from_this();
}

void Statement::dellocate_descriptor(std::shared_ptr<Descriptor> & desc) {
    if (desc) {
        desc->deallocate_self();
        desc.reset();
    }
}
