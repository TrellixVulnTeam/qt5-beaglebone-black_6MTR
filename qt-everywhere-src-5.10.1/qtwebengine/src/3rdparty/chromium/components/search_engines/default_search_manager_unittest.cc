// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/search_engines/default_search_manager.h"

#include <stddef.h>

#include <memory>
#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/search_engines/default_search_manager.h"
#include "components/search_engines/search_engines_pref_names.h"
#include "components/search_engines/search_engines_test_util.h"
#include "components/search_engines/template_url_data.h"
#include "components/search_engines/template_url_data_util.h"
#include "components/search_engines/template_url_prepopulate_data.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// TODO(caitkp): TemplateURLData-ify this.
void SetOverrides(sync_preferences::TestingPrefServiceSyncable* prefs,
                  bool update) {
  prefs->SetUserPref(prefs::kSearchProviderOverridesVersion,
                     base::MakeUnique<base::Value>(1));
  auto overrides = base::MakeUnique<base::ListValue>();
  auto entry = base::MakeUnique<base::DictionaryValue>();

  entry->SetString("name", update ? "new_foo" : "foo");
  entry->SetString("keyword", update ? "new_fook" : "fook");
  entry->SetString("search_url", "http://foo.com/s?q={searchTerms}");
  entry->SetString("favicon_url", "http://foi.com/favicon.ico");
  entry->SetString("encoding", "UTF-8");
  entry->SetInteger("id", 1001);
  entry->SetString("suggest_url", "http://foo.com/suggest?q={searchTerms}");
  entry->SetString("instant_url", "http://foo.com/instant?q={searchTerms}");
  auto alternate_urls = base::MakeUnique<base::ListValue>();
  alternate_urls->AppendString("http://foo.com/alternate?q={searchTerms}");
  entry->Set("alternate_urls", std::move(alternate_urls));
  entry->SetString("search_terms_replacement_key", "espv");
  overrides->Append(std::move(entry));

  entry = base::MakeUnique<base::DictionaryValue>();
  entry->SetInteger("id", 1002);
  entry->SetString("name", update ? "new_bar" : "bar");
  entry->SetString("keyword", update ? "new_bark" : "bark");
  entry->SetString("encoding", std::string());
  overrides->Append(base::MakeUnique<base::Value>(*entry));
  entry->SetInteger("id", 1003);
  entry->SetString("name", "baz");
  entry->SetString("keyword", "bazk");
  entry->SetString("encoding", "UTF-8");
  overrides->Append(std::move(entry));
  prefs->SetUserPref(prefs::kSearchProviderOverrides, std::move(overrides));
}

void SetPolicy(sync_preferences::TestingPrefServiceSyncable* prefs,
               bool enabled,
               TemplateURLData* data) {
  if (enabled) {
    EXPECT_FALSE(data->keyword().empty());
    EXPECT_FALSE(data->url().empty());
  }
  std::unique_ptr<base::DictionaryValue> entry(
      TemplateURLDataToDictionary(*data));
  entry->SetBoolean(DefaultSearchManager::kDisabledByPolicy, !enabled);
  prefs->SetManagedPref(
      DefaultSearchManager::kDefaultSearchProviderDataPrefName,
      std::move(entry));
}

}  // namespace

class DefaultSearchManagerTest : public testing::Test {
 public:
  DefaultSearchManagerTest() {};

  void SetUp() override {
    pref_service_.reset(new sync_preferences::TestingPrefServiceSyncable);
    DefaultSearchManager::RegisterProfilePrefs(pref_service_->registry());
    TemplateURLPrepopulateData::RegisterProfilePrefs(pref_service_->registry());
  }

  sync_preferences::TestingPrefServiceSyncable* pref_service() {
    return pref_service_.get();
  }

 private:
  std::unique_ptr<sync_preferences::TestingPrefServiceSyncable> pref_service_;

  DISALLOW_COPY_AND_ASSIGN(DefaultSearchManagerTest);
};

// Test that a TemplateURLData object is properly written and read from Prefs.
TEST_F(DefaultSearchManagerTest, ReadAndWritePref) {
  DefaultSearchManager manager(pref_service(),
                               DefaultSearchManager::ObserverCallback());
  TemplateURLData data;
  data.SetShortName(base::UTF8ToUTF16("name1"));
  data.SetKeyword(base::UTF8ToUTF16("key1"));
  data.SetURL("http://foo1/{searchTerms}");
  data.suggestions_url = "http://sugg1";
  data.alternate_urls.push_back("http://foo1/alt");
  data.favicon_url = GURL("http://icon1");
  data.safe_for_autoreplace = true;
  data.input_encodings = base::SplitString(
      "UTF-8;UTF-16", ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  data.date_created = base::Time();
  data.last_modified = base::Time();
  data.last_modified = base::Time();

  manager.SetUserSelectedDefaultSearchEngine(data);
  const TemplateURLData* read_data = manager.GetDefaultSearchEngine(nullptr);
  ExpectSimilar(&data, read_data);
}

// Test DefaultSearchmanager handles user-selected DSEs correctly.
TEST_F(DefaultSearchManagerTest, DefaultSearchSetByUserPref) {
  size_t default_search_index = 0;
  DefaultSearchManager manager(pref_service(),
                               DefaultSearchManager::ObserverCallback());
  std::vector<std::unique_ptr<TemplateURLData>> prepopulated_urls =
      TemplateURLPrepopulateData::GetPrepopulatedEngines(pref_service(),
                                                         &default_search_index);
  DefaultSearchManager::Source source = DefaultSearchManager::FROM_POLICY;
  // If no user pref is set, we should use the pre-populated values.
  ExpectSimilar(prepopulated_urls[default_search_index].get(),
                manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_FALLBACK, source);

  // Setting a user pref overrides the pre-populated values.
  std::unique_ptr<TemplateURLData> data = GenerateDummyTemplateURLData("user");
  manager.SetUserSelectedDefaultSearchEngine(*data.get());

  ExpectSimilar(data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_USER, source);

  // Updating the user pref (externally to this instance of
  // DefaultSearchManager) triggers an update.
  std::unique_ptr<TemplateURLData> new_data =
      GenerateDummyTemplateURLData("user2");
  DefaultSearchManager other_manager(pref_service(),
                                     DefaultSearchManager::ObserverCallback());
  other_manager.SetUserSelectedDefaultSearchEngine(*new_data.get());

  ExpectSimilar(new_data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_USER, source);

  // Clearing the user pref should cause the default search to revert to the
  // prepopulated vlaues.
  manager.ClearUserSelectedDefaultSearchEngine();
  ExpectSimilar(prepopulated_urls[default_search_index].get(),
                manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_FALLBACK, source);
}

// Test that DefaultSearch manager detects changes to kSearchProviderOverrides.
TEST_F(DefaultSearchManagerTest, DefaultSearchSetByOverrides) {
  SetOverrides(pref_service(), false);
  size_t default_search_index = 0;
  DefaultSearchManager manager(pref_service(),
                               DefaultSearchManager::ObserverCallback());
  std::vector<std::unique_ptr<TemplateURLData>> prepopulated_urls =
      TemplateURLPrepopulateData::GetPrepopulatedEngines(pref_service(),
                                                         &default_search_index);

  DefaultSearchManager::Source source = DefaultSearchManager::FROM_POLICY;
  TemplateURLData first_default(*manager.GetDefaultSearchEngine(&source));
  ExpectSimilar(prepopulated_urls[default_search_index].get(), &first_default);
  EXPECT_EQ(DefaultSearchManager::FROM_FALLBACK, source);

  // Update the overrides:
  SetOverrides(pref_service(), true);
  prepopulated_urls = TemplateURLPrepopulateData::GetPrepopulatedEngines(
      pref_service(), &default_search_index);

  // Make sure DefaultSearchManager updated:
  ExpectSimilar(prepopulated_urls[default_search_index].get(),
                manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_FALLBACK, source);
  EXPECT_NE(manager.GetDefaultSearchEngine(NULL)->short_name(),
            first_default.short_name());
  EXPECT_NE(manager.GetDefaultSearchEngine(NULL)->keyword(),
            first_default.keyword());
}

// Test DefaultSearchManager handles policy-enforced DSEs correctly.
TEST_F(DefaultSearchManagerTest, DefaultSearchSetByPolicy) {
  DefaultSearchManager manager(pref_service(),
                               DefaultSearchManager::ObserverCallback());
  std::unique_ptr<TemplateURLData> data = GenerateDummyTemplateURLData("user");
  manager.SetUserSelectedDefaultSearchEngine(*data.get());

  DefaultSearchManager::Source source = DefaultSearchManager::FROM_FALLBACK;
  ExpectSimilar(data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_USER, source);

  std::unique_ptr<TemplateURLData> policy_data =
      GenerateDummyTemplateURLData("policy");
  SetPolicy(pref_service(), true, policy_data.get());

  ExpectSimilar(policy_data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_POLICY, source);

  TemplateURLData null_policy_data;
  SetPolicy(pref_service(), false, &null_policy_data);
  EXPECT_EQ(NULL, manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_POLICY, source);

  pref_service()->RemoveManagedPref(
      DefaultSearchManager::kDefaultSearchProviderDataPrefName);
  ExpectSimilar(data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_USER, source);
}

// Test DefaultSearchManager handles extension-controlled DSEs correctly.
TEST_F(DefaultSearchManagerTest, DefaultSearchSetByExtension) {
  DefaultSearchManager manager(pref_service(),
                               DefaultSearchManager::ObserverCallback());
  std::unique_ptr<TemplateURLData> data = GenerateDummyTemplateURLData("user");
  manager.SetUserSelectedDefaultSearchEngine(*data);

  DefaultSearchManager::Source source = DefaultSearchManager::FROM_FALLBACK;
  ExpectSimilar(data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_USER, source);

  // Extension trumps prefs:
  std::unique_ptr<TemplateURLData> extension_data_1 =
      GenerateDummyTemplateURLData("ext1");
  SetExtensionDefaultSearchInPrefs(pref_service(), *extension_data_1);
  ExpectSimilar(extension_data_1.get(),
                manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_EXTENSION, source);

  // Policy trumps extension:
  std::unique_ptr<TemplateURLData> policy_data =
      GenerateDummyTemplateURLData("policy");
  SetPolicy(pref_service(), true, policy_data.get());

  ExpectSimilar(policy_data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_POLICY, source);
  pref_service()->RemoveManagedPref(
      DefaultSearchManager::kDefaultSearchProviderDataPrefName);

  // Extensions trump each other:
  std::unique_ptr<TemplateURLData> extension_data_2 =
      GenerateDummyTemplateURLData("ext2");
  std::unique_ptr<TemplateURLData> extension_data_3 =
      GenerateDummyTemplateURLData("ext3");

  SetExtensionDefaultSearchInPrefs(pref_service(), *extension_data_2);
  SetExtensionDefaultSearchInPrefs(pref_service(), *extension_data_3);
  ExpectSimilar(extension_data_3.get(),
                manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_EXTENSION, source);

  RemoveExtensionDefaultSearchFromPrefs(pref_service());
  ExpectSimilar(data.get(), manager.GetDefaultSearchEngine(&source));
  EXPECT_EQ(DefaultSearchManager::FROM_USER, source);
}
