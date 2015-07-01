#include <mbgl/style/style.hpp>
#include <mbgl/map/sprite.hpp>
#include <mbgl/map/map_data.hpp>
#include <mbgl/map/source.hpp>
#include <mbgl/map/transform_state.hpp>
#include <mbgl/annotation/sprite_store.hpp>
#include <mbgl/annotation/sprite_parser.hpp>
#include <mbgl/style/style_layer.hpp>
#include <mbgl/style/style_parser.hpp>
#include <mbgl/style/style_bucket.hpp>
#include <mbgl/geometry/glyph_atlas.hpp>
#include <mbgl/geometry/sprite_atlas.hpp>
#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/uv_detail.hpp>
#include <mbgl/platform/log.hpp>
#include <csscolorparser/csscolorparser.hpp>

#include <rapidjson/document.h>

#include <algorithm>

namespace mbgl {

Style::Style(const std::string& json, const std::string&, MapData& data_,
             uv_loop_t* loop)
    : data(data_),
      glyphStore(std::make_unique<GlyphStore>(loop)),
      glyphAtlas(std::make_unique<GlyphAtlas>(1024, 1024)),
      spriteStore(std::make_unique<SpriteStore>(data.pixelRatio)),
      spriteAtlas(std::make_unique<SpriteAtlas>(512, 512, *spriteStore)),
      lineAtlas(std::make_unique<LineAtlas>(512, 512)),
      mtx(std::make_unique<uv::rwlock>()),
      workers(4) {

    rapidjson::Document doc;
    doc.Parse<0>((const char *const)json.c_str());
    if (doc.HasParseError()) {
        Log::Error(Event::ParseStyle, "Error parsing style JSON at %i: %s", doc.GetErrorOffset(), doc.GetParseError());
        return;
    }

    StyleParser parser;
    parser.parse(doc);

    sources = parser.getSources();
    layers = parser.getLayers();

    sprite = std::make_unique<Sprite>(parser.getSprite(), data.pixelRatio);
    sprite->setObserver(this);

    glyphStore->setURL(parser.getGlyphURL());

    for (const auto& source : sources) {
        source->setObserver(this);
        source->load();
    }

    glyphStore->setObserver(this);
}

Style::~Style() {
    for (const auto& source : sources) {
        source->setObserver(nullptr);
    }

    glyphStore->setObserver(nullptr);

    if (sprite) {
        sprite->setObserver(nullptr);
    }
}

void Style::update(const TransformState& transform,
                   TexturePool& texturePool) {
    bool allTilesUpdated = true;
    for (const auto& source : sources) {
        if (!source->update(data, transform, *this, texturePool, shouldReparsePartialTiles)) {
            allTilesUpdated = false;
        }
    }

    // We can only stop updating "partial" tiles when all of them
    // were notified of the arrival of the new resources.
    if (allTilesUpdated) {
        shouldReparsePartialTiles = false;
    }
}

void Style::cascade(const std::vector<std::string>& classes) {
    TimePoint now = Clock::now();

    for (const auto& layer : layers) {
        layer->setClasses(classes, now, defaultTransition);
    }
}

void Style::recalculate(float z, TimePoint now) {
    uv::writelock lock(mtx);

    for (const auto& source : sources) {
        source->enabled = false;
    }

    zoomHistory.update(z, now);

    for (const auto& layer : layers) {
        layer->updateProperties(z, now, zoomHistory);
        if (!layer->bucket) {
            continue;
        }

        Source* source = getSource(layer->bucket->source);
        if (!source) {
            continue;
        }

        source->enabled = true;
    }
}

Source* Style::getSource(const std::string& id) const {
    const auto it = std::find_if(sources.begin(), sources.end(), [&](const auto& source) {
        return source->info.source_id == id;
    });

    return it != sources.end() ? it->get() : nullptr;
}

void Style::setDefaultTransitionDuration(Duration duration) {
    defaultTransition.duration = duration;
}

bool Style::hasTransitions() const {
    for (const auto& layer : layers) {
        if (layer->hasTransitions()) {
            return true;
        }
    }
    return false;
}

bool Style::isLoaded() const {
    for (const auto& source : sources) {
        if (!source->isLoaded()) {
            return false;
        }
    }

    if (sprite && !sprite->isLoaded()) {
        return false;
    }

    return true;
}

void Style::setObserver(Observer* observer_) {
    assert(util::ThreadContext::currentlyOn(util::ThreadType::Map));
    assert(!observer);

    observer = observer_;
}

void Style::onGlyphRangeLoaded() {
    shouldReparsePartialTiles = true;

    emitTileDataChanged();
}

void Style::onGlyphRangeLoadingFailed(std::exception_ptr error) {
    emitResourceLoadingFailed(error);
}

void Style::onSourceLoaded() {
    emitTileDataChanged();
}

void Style::onSourceLoadingFailed(std::exception_ptr error) {
    emitResourceLoadingFailed(error);
}

void Style::onTileLoaded(bool isNewTile) {
    if (isNewTile) {
        shouldReparsePartialTiles = true;
    }

    emitTileDataChanged();
}

void Style::onTileLoadingFailed(std::exception_ptr error) {
    emitResourceLoadingFailed(error);
}

void Style::onSpriteLoaded() {
    // Add all sprite images to the SpriteStore object
    spriteStore->setSprites(parseSprite(sprite->getImage(), sprite->getJSON()));

    shouldReparsePartialTiles = true;

    emitTileDataChanged();
}

void Style::onSpriteLoadingFailed(std::exception_ptr error) {
    emitResourceLoadingFailed(error);
}

void Style::emitTileDataChanged() {
    assert(util::ThreadContext::currentlyOn(util::ThreadType::Map));

    if (observer) {
        observer->onTileDataChanged();
    }
}

void Style::emitResourceLoadingFailed(std::exception_ptr error) {
    assert(util::ThreadContext::currentlyOn(util::ThreadType::Map));

    try {
        if (error) {
            lastError = error;
            std::rethrow_exception(error);
        }
    } catch(const std::exception& e) {
        Log::Error(Event::Style, e.what());
    }

    if (observer) {
        observer->onResourceLoadingFailed(error);
    }
}

}
