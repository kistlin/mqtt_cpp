// Copyright Wouter van Kleunen 2021
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(MQTT_BROKER_SECURITY_HPP)
#define MQTT_BROKER_SECURITY_HPP

#include <string>

#include <mqtt/broker/broker_namespace.hpp>
#include <mqtt/broker/subscription_map.hpp>
#include <mqtt/optional.hpp>

#include <map>
#include <set>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/optional.hpp>

#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string.hpp>

#if MQTT_USE_TLS
#include <openssl/evp.h>
#endif

MQTT_BROKER_NS_BEGIN

struct security
{

    struct authentication
    {
        enum class method {
            password,
            client_cert,
            anonymous
        };

        method method_;
        optional<std::string> password;

        authentication(method method_ = method::password, optional<std::string> const &password = optional<std::string>())
            : method_(method_), password(password)
        { }

        std::vector<std::string> groups;
    };


    struct authorization
    {        
        enum class type {
            deny, allow
        };

        type sub_type;
        std::vector<std::string> sub;

        type pub_type;
        std::vector<std::string> pub;

        authorization()
            : sub_type(type::deny), pub_type(type::deny)
        { }
    };

    struct group
    {
        std::string name;
        std::vector<std::string> members;
    };


    optional<std::string> login_anonymous() {
        return anonymous;
    }

    template<typename T>
    static inline std::string to_hex(T const start, T const end)
    {
        std::string result;
        boost::algorithm::hex(start, end, std::back_inserter(result));
        return result;
    }

#if defined(MQTT_USE_TLS)
    static inline std::string hash(std::string const &message)
    {
        std::shared_ptr<EVP_MD_CTX> mdctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        EVP_DigestInit_ex(mdctx.get(), EVP_sha256(), NULL);
        EVP_DigestUpdate(mdctx.get(), message.data(), message.size());

        std::vector<unsigned char> digest(static_cast<std::size_t>(EVP_MD_size(EVP_sha256())));
        unsigned int digest_size = static_cast<unsigned int>(digest.size());

        EVP_DigestFinal_ex(mdctx.get(), digest.data(), &digest_size);
        return to_hex(digest.data(), digest.data() + digest_size);
    }
#else
    static inline std::string hash(std::string const &message)
    {
        return message;
    }
#endif

    bool login_cert(string_view const& username) const {
        auto i = authentication_.find(std::string(username));
        return (i != authentication_.end() && i->second.method_ == security::authentication::method::client_cert);
    }

    optional<std::string> login(string_view const& username, string_view const& password) const {
        optional<std::string> empty_result;
        auto i = authentication_.find(std::string(username));
        if (i == authentication_.end() || i->second.method_ != security::authentication::method::password)
            return empty_result;
        return boost::iequals(*(i->second.password), hash(hash_type + ":" + salt + ":" + std::string(password))) ? std::string(username) : empty_result;
    }

    static authorization::type get_auth_type(string_view const& type) {
        if(type == "allow") return authorization::type::allow;
        if(type == "deny") return authorization::type::deny;
        throw std::runtime_error("An invalid authorization type was specified: " + std::string(type));
    }

    static bool is_valid_group_name(string_view const& name) {
        return !name.empty() && name[0] == '@'; // TODO: validate utf-8
    }

    static bool is_valid_user_name(string_view const& name) {
        return !name.empty() && name[0] != '@'; // TODO: validate utf-8
    }

    void default_config()
    {
        hash_type = "aes256";
        salt = "salt";

        char const *username = "anonymous";
        authentication login(authentication::method::anonymous);
        authentication_.insert({ username, login});
        anonymous = username;

        char const *topic = "#";
        authorization auth;
        auth.sub_type = authorization::type::allow;
        auth.sub.push_back(username);
        auth.pub_type = authorization::type::allow;
        auth.pub.push_back(username);
        authorization_.insert({ topic, auth });

        validate();
    }

    void load_json(std::istream& input) {
        // Create a root
        boost::property_tree::ptree root;
        boost::property_tree::read_json(input, root);

        for(auto const &i: root.get_child("authentication")) {
            std::string name = i.second.get<std::string>("name");
            if(!is_valid_user_name(name)) throw std::runtime_error("An invalid username was specified: " + name);

            std::string method = i.second.get<std::string>("method");

            if(method == "password") {
                std::string password = i.second.get<std::string>("password");
                authentication auth(authentication::method::password, password);
                authentication_.insert( { name, auth });
            } else if(method == "client_cert") {
                authentication auth(authentication::method::client_cert);
                authentication_.insert({ name, auth });
            } else if(method == "anonymous") {
                if(anonymous) throw std::runtime_error("Only a single anonymous user can be configured, anonymous user: " + *anonymous);
                anonymous = name;

                authentication auth(authentication::method::anonymous);
                authentication_.insert( { name, auth });
            } else throw std::runtime_error("An invalid method was specified: " + method);
        }
        for(auto const& i: root.get_child("group")) {
            std::string name = i.second.get<std::string>("name");
            if(!is_valid_group_name(name)) throw std::runtime_error("An invalid group name was specified: " + name);

            group group;
            if(i.second.get_child_optional("members")) {
                for(auto const& j: i.second.get_child("members")) {
                    auto username = j.second.get_value<std::string>();
                    if(!is_valid_user_name(username)) throw std::runtime_error("An invalid user name was specified: " + username);
                    group.members.push_back(username);
                }
            }

            groups_.insert({ name, group });
        }

        for(auto const& i: root.get_child("authorization")) {
            std::string name = i.second.get<std::string>("topic");
            //if(!is_valid_topic(name)) throw std::runtime_error("An invalid topic was specified: " + name);

            auto type = get_auth_type(i.second.get<std::string>("type"));

            authorization &auth = authorization_[name];
            if(i.second.get_child_optional("sub")) {
                for(auto const& j: i.second.get_child("sub"))
                    auth.sub.push_back(j.second.get_value<std::string>());
                auth.sub_type = type;
            }

            if(i.second.get_child_optional("pub")) {
                for(auto const& j: i.second.get_child("pub"))
                    auth.pub.push_back(j.second.get_value<std::string>());
                auth.pub_type = type;
            }            
        }

        hash_type = root.get<std::string>("config.hash");
        salt = root.get<std::string>("config.salt");
        validate();
    }

    std::vector<std::pair<std::string, authorization::type>> get_auth_sub_by_user(string_view username) const {
        std::set<std::string> username_and_groups;
        username_and_groups.insert(std::string(username));

        for (auto const &i: groups_) {
            if (std::find(i.second.members.begin(), i.second.members.end(), username) != i.second.members.end()) {
                username_and_groups.insert(i.first);
            }
        }

        std::vector<std::pair<std::string, authorization::type>> result;
        for(auto const &i: authorization_) {
            for (auto const &j: i.second.sub) {
                if (username_and_groups.find(j) != username_and_groups.end()) {
                    result.push_back(std::make_pair(i.first, i.second.sub_type));
                }
            }
        }

        return result;
    }

    authorization::type auth_pub(string_view const& topic, string_view const& username) const {
        authorization::type result_type = authorization::type::deny;

        auth_pub_map.find(topic, [&](std::string const &allowed_username, authorization::type type) {
            if (allowed_username == username) result_type = type;
        });

        return result_type;
    }

    std::map<std::string, authorization::type> auth_sub(string_view const& topic) const {
        std::map<std::string, authorization::type> result;

        auth_sub_map.find(topic, [&](std::string const &allowed_username, authorization::type type) {
            result[allowed_username] = type;
        });

        return result;
    }

    static authorization::type auth_sub_user(std::map<std::string, authorization::type> const& result, std::string const& username) {
        auto i = result.find(username);
        if(i == result.end()) return authorization::type::deny;
        return i->second;
    }

    std::map<std::string, authentication> authentication_;
    std::map<std::string, group> groups_;
    std::map<std::string, authorization> authorization_;
    optional<std::string> anonymous;

    using auth_map_type = multiple_subscription_map<std::string, authorization::type>;
    auth_map_type auth_pub_map;
    auth_map_type auth_sub_map;

    std::string salt;
    std::string hash_type;

private:
    void validate_entry(std::string const& context, std::string const& name) {
        if(is_valid_group_name(name) && groups_.find(name) == groups_.end())
            throw std::runtime_error("An invalid group name was specified for " + context + ": " + name);
        if(is_valid_user_name(name) && authentication_.find(name) == authentication_.end())
            throw std::runtime_error("An invalid username name was specified for " + context + ": " + name);
    }

    void validate()
    {
        if (hash_type != "aes256")
            throw std::runtime_error("An invalid hash type was selected: " + hash_type);

        for(auto const& i: groups_) {
            for(auto const& j: i.second.members) {
                auto iter = authentication_.find(j);
                if(is_valid_user_name(j) && iter == authentication_.end())
                    throw std::runtime_error("An invalid username name was specified for group " + i.first + ": " + j);
            }
        }

        for(auto const &i: authorization_) {
            for(auto const& j: i.second.sub) {
                validate_entry("topic " + i.first, j);

                if(is_valid_user_name(j)) {
                    auth_sub_map.insert_or_assign(i.first, j, i.second.sub_type);
                }
                else if(is_valid_group_name(j)) {
                    for(auto const& z: groups_[j].members)
                        auth_sub_map.insert_or_assign(i.first, z, i.second.sub_type);
                }
            }
            for(auto const& j: i.second.pub) {
                validate_entry("topic " + i.first, j);

                if(is_valid_user_name(j)) {
                    auth_pub_map.insert_or_assign(i.first, j, i.second.pub_type);
                }
                else if(is_valid_group_name(j)) {
                    for(auto const& z: groups_[j].members)
                        auth_pub_map.insert_or_assign(i.first, z, i.second.pub_type);
                }
            }
        }
    }   
};

MQTT_BROKER_NS_END

#endif // MQTT_BROKER_SECURITY_HPP
