#include <algorithm>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#if defined(GR4CP_HAVE_GNURADIO4)

#include "gr4cp/catalog/gr4_block_catalog_provider.hpp"
#include "gr4cp/catalog/gr4_block_catalog_provider_detail.hpp"

namespace {

TEST(Gr4BlockCatalogProviderTest, FallbackCategoryDerivationPreservesNamespacePath) {
    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_block_id("gr::incubator::analog::QuadratureDemod"),
              "incubator/analog");
    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_block_id("gr::incubator::http::HttpTimeSeriesSink"),
              "incubator/http");
    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_block_id("gr::basic::SignalGenerator<float32>"),
              "basic");
    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_block_id("gr::studio::StudioSeriesSink<float32>"),
              "studio");
}

TEST(Gr4BlockCatalogProviderTest, ExplicitCategoryMetadataOverridesFallbackDerivation) {
    gr::property_map meta;
    meta["Drawable"] = gr::property_map{{"Category", "Toolbar"}};

    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_metadata(
                  meta,
                  "gr::incubator::analog::QuadratureDemod"),
              "Toolbar");
}

TEST(Gr4BlockCatalogProviderTest, MalformedExplicitCategoryMetadataFallsBackToDerivedPath) {
    gr::property_map meta;
    meta["Drawable"] = gr::property_map{{"Category", "basic/ClockSource<uint8,float>"}};

    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_metadata(
                  meta,
                  "gr::incubator::analog::QuadratureDemod"),
              "incubator/analog");
}

TEST(Gr4BlockCatalogProviderTest, TooShortIdsFallbackToUncategorized) {
    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_block_id("gr"), "Uncategorized");
    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_block_id("gr::OnlyType"), "Uncategorized");
    EXPECT_EQ(gr4cp::catalog::detail::derive_category_from_block_id("OnlyType"), "Uncategorized");
}

TEST(Gr4BlockCatalogProviderTest, ListLoadsCatalogFromGNUradio4) {
    gr4cp::catalog::Gr4BlockCatalogProvider provider;

    std::vector<gr4cp::domain::BlockDescriptor> blocks;
    try {
        blocks = provider.list();
    } catch (const gr4cp::catalog::CatalogLoadError& error) {
        GTEST_SKIP() << error.what();
    }

    ASSERT_FALSE(blocks.empty());
    EXPECT_FALSE(blocks.front().id.empty());
    EXPECT_FALSE(blocks.front().name.empty());
    EXPECT_FALSE(blocks.front().category.empty());
}

TEST(Gr4BlockCatalogProviderTest, CatalogContainsStableBlockShape) {
    gr4cp::catalog::Gr4BlockCatalogProvider provider;

    std::vector<gr4cp::domain::BlockDescriptor> blocks;
    try {
        blocks = provider.list();
    } catch (const gr4cp::catalog::CatalogLoadError& error) {
        GTEST_SKIP() << error.what();
    }
    const auto it = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) {
        return !block.inputs.empty() || !block.outputs.empty() || !block.parameters.empty();
    });

    ASSERT_NE(it, blocks.end());
    EXPECT_FALSE(it->id.empty());
    EXPECT_FALSE(it->name.empty());
    EXPECT_FALSE(it->category.empty());
}

TEST(Gr4BlockCatalogProviderTest, ProviderUsesNamespacePathFallbackCategoryWhenNoExplicitMetadataExists) {
    gr4cp::catalog::Gr4BlockCatalogProvider provider;

    std::vector<gr4cp::domain::BlockDescriptor> blocks;
    try {
        blocks = provider.list();
    } catch (const gr4cp::catalog::CatalogLoadError& error) {
        GTEST_SKIP() << error.what();
    }

    const auto signal_generator = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) {
        return block.id == "gr::blocks::basic::SignalGenerator<float32>";
    });
    ASSERT_NE(signal_generator, blocks.end());
    EXPECT_EQ(signal_generator->category, "blocks/basic");
}

TEST(Gr4BlockCatalogProviderTest, BlockDetailsIncludeBuiltinParametersAndExtendedMetadata) {
    gr4cp::catalog::Gr4BlockCatalogProvider provider;

    std::vector<gr4cp::domain::BlockDescriptor> blocks;
    try {
        blocks = provider.list();
    } catch (const gr4cp::catalog::CatalogLoadError& error) {
        GTEST_SKIP() << error.what();
    }

    const auto block_it = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) {
        return block.id == "gr::blocks::basic::SignalGenerator<float32>";
    });
    ASSERT_NE(block_it, blocks.end());
    EXPECT_FALSE(block_it->summary.empty());

    const auto parameter_by_name = [&](std::string_view name) -> const gr4cp::domain::BlockParameterDescriptor* {
        const auto it = std::find_if(block_it->parameters.begin(), block_it->parameters.end(), [&](const auto& parameter) {
            return parameter.name == name;
        });
        return it == block_it->parameters.end() ? nullptr : &*it;
    };

    const auto* unique_name = parameter_by_name("unique_name");
    const auto* compute_domain = parameter_by_name("compute_domain");
    const auto* disconnect_on_done = parameter_by_name("disconnect_on_done");
    const auto* input_chunk_size = parameter_by_name("input_chunk_size");
    const auto* output_chunk_size = parameter_by_name("output_chunk_size");
    const auto* stride = parameter_by_name("stride");
    const auto* name = parameter_by_name("name");
    const auto* ui_constraints = parameter_by_name("ui_constraints");

    ASSERT_NE(unique_name, nullptr);
    ASSERT_NE(compute_domain, nullptr);
    ASSERT_NE(disconnect_on_done, nullptr);
    ASSERT_NE(input_chunk_size, nullptr);
    ASSERT_NE(output_chunk_size, nullptr);
    ASSERT_NE(stride, nullptr);
    ASSERT_NE(name, nullptr);
    ASSERT_NE(ui_constraints, nullptr);

    EXPECT_EQ(unique_name->runtime_mutability, std::optional<std::string>("immutable"));
    EXPECT_FALSE(compute_domain->runtime_mutability.has_value());
    EXPECT_FALSE(disconnect_on_done->runtime_mutability.has_value());
    EXPECT_FALSE(input_chunk_size->runtime_mutability.has_value());
    EXPECT_FALSE(output_chunk_size->runtime_mutability.has_value());
    EXPECT_FALSE(stride->runtime_mutability.has_value());
    EXPECT_EQ(name->runtime_mutability, std::optional<std::string>("immutable"));
    EXPECT_FALSE(ui_constraints->runtime_mutability.has_value());

    EXPECT_EQ(unique_name->ui_hint, std::optional<std::string>("read_only"));
    EXPECT_EQ(compute_domain->ui_hint, std::optional<std::string>("advanced"));
    EXPECT_EQ(disconnect_on_done->ui_hint, std::optional<std::string>("advanced"));
    EXPECT_EQ(input_chunk_size->ui_hint, std::optional<std::string>("advanced"));
    EXPECT_EQ(output_chunk_size->ui_hint, std::optional<std::string>("advanced"));
    EXPECT_EQ(stride->ui_hint, std::optional<std::string>("advanced"));
    EXPECT_EQ(name->ui_hint, std::optional<std::string>("read_only"));
    EXPECT_EQ(ui_constraints->ui_hint, std::optional<std::string>("advanced"));

    const auto* amplitude = parameter_by_name("amplitude");
    const auto* signal_type = parameter_by_name("signal_type");
    ASSERT_NE(amplitude, nullptr);
    ASSERT_NE(signal_type, nullptr);
    EXPECT_FALSE(amplitude->runtime_mutability.has_value());
    EXPECT_TRUE(amplitude->value_kind == std::optional<std::string>("scalar") || !amplitude->value_kind.has_value());
    EXPECT_FALSE(amplitude->enum_choices.has_value());
    EXPECT_FALSE(amplitude->enum_type.has_value());
    EXPECT_TRUE(amplitude->enum_labels.empty());
    EXPECT_FALSE(amplitude->enum_source.has_value());
    EXPECT_FALSE(amplitude->allow_custom_value.has_value());

    EXPECT_EQ(signal_type->type, "string");
    EXPECT_EQ(signal_type->enum_choices,
              std::optional<std::vector<std::string>>(
                  std::vector<std::string>{"Const", "Sin", "Cos", "Square", "Saw", "Triangle", "FastSin", "FastCos", "UniformNoise", "TriangularNoise", "GaussianNoise"}));
    EXPECT_TRUE(signal_type->enum_type.has_value());
    EXPECT_FALSE(signal_type->enum_type->empty());
}

TEST(Gr4BlockCatalogProviderTest, DynamicPortCollectionsExposeCollectionMetadata) {
    gr4cp::catalog::Gr4BlockCatalogProvider provider;

    std::vector<gr4cp::domain::BlockDescriptor> blocks;
    try {
        blocks = provider.list();
    } catch (const gr4cp::catalog::CatalogLoadError& error) {
        GTEST_SKIP() << error.what();
    }

    const auto block_it = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) {
        return block.id.find("Add<") != std::string::npos &&
               std::any_of(block.inputs.begin(), block.inputs.end(), [](const auto& port) {
                   return port.cardinality_kind == gr4cp::domain::BlockPortCardinalityKind::Dynamic;
               });
    });
    ASSERT_NE(block_it, blocks.end());

    const auto port_it = std::find_if(block_it->inputs.begin(), block_it->inputs.end(), [](const auto& port) {
        return port.cardinality_kind == gr4cp::domain::BlockPortCardinalityKind::Dynamic;
    });
    ASSERT_NE(port_it, block_it->inputs.end());
    EXPECT_EQ(port_it->name, "in");
    EXPECT_FALSE(port_it->type.empty());
    EXPECT_EQ(port_it->cardinality_kind, gr4cp::domain::BlockPortCardinalityKind::Dynamic);
    EXPECT_TRUE(port_it->current_port_count.has_value());
    EXPECT_TRUE(port_it->render_port_count.has_value());
    EXPECT_TRUE(port_it->min_port_count.has_value());
    EXPECT_TRUE(port_it->max_port_count.has_value());
    EXPECT_TRUE(port_it->size_parameter.has_value());
    EXPECT_TRUE(port_it->handle_name_template.has_value());
}

}  // namespace

#endif
