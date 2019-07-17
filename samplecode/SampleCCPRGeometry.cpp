/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkTypes.h"

#if SK_SUPPORT_GPU

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "samplecode/Sample.h"
#include "src/core/SkMakeUnique.h"
#include "src/core/SkRectPriv.h"
#include "src/gpu/GrClip.h"
#include "src/gpu/GrContextPriv.h"
#include "src/gpu/GrMemoryPool.h"
#include "src/gpu/GrRenderTargetContext.h"
#include "src/gpu/GrRenderTargetContextPriv.h"
#include "src/gpu/GrResourceProvider.h"
#include "src/gpu/ccpr/GrCCCoverageProcessor.h"
#include "src/gpu/ccpr/GrCCFillGeometry.h"
#include "src/gpu/ccpr/GrCCStroker.h"
#include "src/gpu/ccpr/GrGSCoverageProcessor.h"
#include "src/gpu/ccpr/GrVSCoverageProcessor.h"
#include "src/gpu/geometry/GrPathUtils.h"
#include "src/gpu/gl/GrGLGpu.h"
#include "src/gpu/glsl/GrGLSLFragmentProcessor.h"
#include "src/gpu/ops/GrDrawOp.h"

using TriPointInstance = GrCCCoverageProcessor::TriPointInstance;
using QuadPointInstance = GrCCCoverageProcessor::QuadPointInstance;
using PrimitiveType = GrCCCoverageProcessor::PrimitiveType;

static constexpr float kDebugBloat = 40;

/**
 * This sample visualizes the AA bloat geometry generated by the ccpr geometry shaders. It
 * increases the AA bloat by 50x and outputs color instead of coverage (coverage=+1 -> green,
 * coverage=0 -> black, coverage=-1 -> red). Use the keys 1-7 to cycle through the different
 * geometry processors.
 */
class CCPRGeometryView : public Sample {
    void onOnceBeforeDraw() override { this->updateGpuData(); }
    void onDrawContent(SkCanvas*) override;

    Sample::Click* onFindClickHandler(SkScalar x, SkScalar y, ModifierKey) override;
    bool onClick(Sample::Click*) override;
    bool onChar(SkUnichar) override;
    SkString name() override { return SkString("CCPRGeometry"); }

    class Click;
    class DrawCoverageCountOp;
    class VisualizeCoverageCountFP;

    void updateAndInval() { this->updateGpuData(); }

    void updateGpuData();

    PrimitiveType fPrimitiveType = PrimitiveType::kTriangles;
    SkCubicType fCubicType;
    SkMatrix fCubicKLM;

    SkPoint fPoints[4] = {
            {100.05f, 100.05f}, {400.75f, 100.05f}, {400.75f, 300.95f}, {100.05f, 300.95f}};

    float fConicWeight = .5;
    float fStrokeWidth = 40;
    bool fDoStroke = false;

    SkTArray<TriPointInstance> fTriPointInstances;
    SkTArray<QuadPointInstance> fQuadPointInstances;
    SkPath fPath;
};

class CCPRGeometryView::DrawCoverageCountOp : public GrDrawOp {
    DEFINE_OP_CLASS_ID

public:
    DrawCoverageCountOp(CCPRGeometryView* view) : INHERITED(ClassID()), fView(view) {
        this->setBounds(SkRect::MakeIWH(fView->width(), fView->height()), GrOp::HasAABloat::kNo,
                        GrOp::IsZeroArea::kNo);
    }

    const char* name() const override {
        return "[Testing/Sample code] CCPRGeometryView::DrawCoverageCountOp";
    }

private:
    FixedFunctionFlags fixedFunctionFlags() const override { return FixedFunctionFlags::kNone; }
    GrProcessorSet::Analysis finalize(const GrCaps&, const GrAppliedClip*,
                                      bool hasMixedSampledCoverage, GrClampType) override {
        return GrProcessorSet::EmptySetAnalysis();
    }
    void onPrepare(GrOpFlushState*) override {}
    void onExecute(GrOpFlushState*, const SkRect& chainBounds) override;

    CCPRGeometryView* fView;

    typedef GrDrawOp INHERITED;
};

class CCPRGeometryView::VisualizeCoverageCountFP : public GrFragmentProcessor {
public:
    VisualizeCoverageCountFP() : GrFragmentProcessor(kTestFP_ClassID, kNone_OptimizationFlags) {}

private:
    const char* name() const override {
        return "[Testing/Sample code] CCPRGeometryView::VisualizeCoverageCountFP";
    }
    std::unique_ptr<GrFragmentProcessor> clone() const override {
        return skstd::make_unique<VisualizeCoverageCountFP>();
    }
    void onGetGLSLProcessorKey(const GrShaderCaps&, GrProcessorKeyBuilder*) const override {}
    bool onIsEqual(const GrFragmentProcessor&) const override { return true; }

    class Impl : public GrGLSLFragmentProcessor {
        void emitCode(EmitArgs& args) override {
            GrGLSLFPFragmentBuilder* f = args.fFragBuilder;
            f->codeAppendf("half count = %s.a;", args.fInputColor);
            f->codeAppendf("%s = half4(clamp(-count, 0, 1), clamp(+count, 0, 1), 0, abs(count));",
                           args.fOutputColor);
        }
    };

    GrGLSLFragmentProcessor* onCreateGLSLInstance() const override { return new Impl; }
};

static void draw_klm_line(int w, int h, SkCanvas* canvas, const SkScalar line[3], SkColor color) {
    SkPoint p1, p2;
    if (SkScalarAbs(line[1]) > SkScalarAbs(line[0])) {
        // Draw from vertical edge to vertical edge.
        p1 = {0, -line[2] / line[1]};
        p2 = {(SkScalar)w, (-line[2] - w * line[0]) / line[1]};
    } else {
        // Draw from horizontal edge to horizontal edge.
        p1 = {-line[2] / line[0], 0};
        p2 = {(-line[2] - h * line[1]) / line[0], (SkScalar)h};
    }

    SkPaint linePaint;
    linePaint.setColor(color);
    linePaint.setAlpha(128);
    linePaint.setStyle(SkPaint::kStroke_Style);
    linePaint.setStrokeWidth(0);
    linePaint.setAntiAlias(true);
    canvas->drawLine(p1, p2, linePaint);
}

void CCPRGeometryView::onDrawContent(SkCanvas* canvas) {
    canvas->clear(SK_ColorBLACK);

    if (!fDoStroke) {
        SkPaint outlinePaint;
        outlinePaint.setColor(0x80ffffff);
        outlinePaint.setStyle(SkPaint::kStroke_Style);
        outlinePaint.setStrokeWidth(0);
        outlinePaint.setAntiAlias(true);
        canvas->drawPath(fPath, outlinePaint);
    }

#if 0
    SkPaint gridPaint;
    gridPaint.setColor(0x10000000);
    gridPaint.setStyle(SkPaint::kStroke_Style);
    gridPaint.setStrokeWidth(0);
    gridPaint.setAntiAlias(true);
    for (int y = 0; y < this->height(); y += kDebugBloat) {
        canvas->drawLine(0, y, this->width(), y, gridPaint);
    }
    for (int x = 0; x < this->width(); x += kDebugBloat) {
        canvas->drawLine(x, 0, x, this->height(), outlinePaint);
    }
#endif

    SkString caption;
    if (GrRenderTargetContext* rtc = canvas->internal_private_accessTopLayerRenderTargetContext()) {
        // Render coverage count.
        GrContext* ctx = canvas->getGrContext();
        SkASSERT(ctx);

        GrOpMemoryPool* pool = ctx->priv().opMemoryPool();

        sk_sp<GrRenderTargetContext> ccbuff = ctx->priv().makeDeferredRenderTargetContext(
                SkBackingFit::kApprox, this->width(), this->height(), GrColorType::kAlpha_F16,
                nullptr);
        SkASSERT(ccbuff);
        ccbuff->clear(nullptr, SK_PMColor4fTRANSPARENT,
                      GrRenderTargetContext::CanClearFullscreen::kYes);
        ccbuff->priv().testingOnly_addDrawOp(pool->allocate<DrawCoverageCountOp>(this));

        // Visualize coverage count in main canvas.
        GrPaint paint;
        paint.addColorFragmentProcessor(
                GrSimpleTextureEffect::Make(sk_ref_sp(ccbuff->asTextureProxy()), SkMatrix::I()));
        paint.addColorFragmentProcessor(
                skstd::make_unique<VisualizeCoverageCountFP>());
        paint.setPorterDuffXPFactory(SkBlendMode::kSrcOver);
        rtc->drawRect(GrNoClip(), std::move(paint), GrAA::kNo, SkMatrix::I(),
                      SkRect::MakeIWH(this->width(), this->height()));

        // Add label.
        caption.appendf("PrimitiveType_%s",
                        GrCCCoverageProcessor::PrimitiveTypeName(fPrimitiveType));
        if (PrimitiveType::kCubics == fPrimitiveType) {
            caption.appendf(" (%s)", SkCubicTypeName(fCubicType));
        } else if (PrimitiveType::kConics == fPrimitiveType) {
            caption.appendf(" (w=%f)", fConicWeight);
        }
        if (fDoStroke) {
            caption.appendf(" (stroke_width=%f)", fStrokeWidth);
        }
    } else {
        caption = "Use GPU backend to visualize geometry.";
    }

    SkPaint pointsPaint;
    pointsPaint.setColor(SK_ColorBLUE);
    pointsPaint.setStrokeWidth(8);
    pointsPaint.setAntiAlias(true);

    if (PrimitiveType::kCubics == fPrimitiveType) {
        canvas->drawPoints(SkCanvas::kPoints_PointMode, 4, fPoints, pointsPaint);
        if (!fDoStroke) {
            int w = this->width(), h = this->height();
            draw_klm_line(w, h, canvas, &fCubicKLM[0], SK_ColorYELLOW);
            draw_klm_line(w, h, canvas, &fCubicKLM[3], SK_ColorBLUE);
            draw_klm_line(w, h, canvas, &fCubicKLM[6], SK_ColorRED);
        }
    } else {
        canvas->drawPoints(SkCanvas::kPoints_PointMode, 2, fPoints, pointsPaint);
        canvas->drawPoints(SkCanvas::kPoints_PointMode, 1, fPoints + 3, pointsPaint);
    }

    SkFont font(nullptr, 20);
    SkPaint captionPaint;
    captionPaint.setColor(SK_ColorWHITE);
    canvas->drawString(caption, 10, 30, font, captionPaint);
}

void CCPRGeometryView::updateGpuData() {
    using Verb = GrCCFillGeometry::Verb;
    fTriPointInstances.reset();
    fQuadPointInstances.reset();

    fPath.reset();
    fPath.moveTo(fPoints[0]);

    if (PrimitiveType::kCubics == fPrimitiveType) {
        double t[2], s[2];
        fCubicType = GrPathUtils::getCubicKLM(fPoints, &fCubicKLM, t, s);
        GrCCFillGeometry geometry;
        geometry.beginContour(fPoints[0]);
        geometry.cubicTo(fPoints, kDebugBloat / 2, kDebugBloat / 2);
        geometry.endContour();
        int ptsIdx = 0;
        for (Verb verb : geometry.verbs()) {
            switch (verb) {
                case Verb::kLineTo:
                    ++ptsIdx;
                    continue;
                case Verb::kMonotonicQuadraticTo:
                    ptsIdx += 2;
                    continue;
                case Verb::kMonotonicCubicTo:
                    fQuadPointInstances.push_back().set(&geometry.points()[ptsIdx], 0, 0);
                    ptsIdx += 3;
                    continue;
                default:
                    continue;
            }
        }
        fPath.cubicTo(fPoints[1], fPoints[2], fPoints[3]);
    } else if (PrimitiveType::kTriangles != fPrimitiveType) {
        SkPoint P3[3] = {fPoints[0], fPoints[1], fPoints[3]};
        GrCCFillGeometry geometry;
        geometry.beginContour(P3[0]);
        if (PrimitiveType::kQuadratics == fPrimitiveType) {
            geometry.quadraticTo(P3);
            fPath.quadTo(fPoints[1], fPoints[3]);
        } else {
            SkASSERT(PrimitiveType::kConics == fPrimitiveType);
            geometry.conicTo(P3, fConicWeight);
            fPath.conicTo(fPoints[1], fPoints[3], fConicWeight);
        }
        geometry.endContour();
        int ptsIdx = 0, conicWeightIdx = 0;
        for (Verb verb : geometry.verbs()) {
            if (Verb::kBeginContour == verb ||
                Verb::kEndOpenContour == verb ||
                Verb::kEndClosedContour == verb) {
                continue;
            }
            if (Verb::kLineTo == verb) {
                ++ptsIdx;
                continue;
            }
            SkASSERT(Verb::kMonotonicQuadraticTo == verb || Verb::kMonotonicConicTo == verb);
            if (PrimitiveType::kQuadratics == fPrimitiveType &&
                Verb::kMonotonicQuadraticTo == verb) {
                fTriPointInstances.push_back().set(&geometry.points()[ptsIdx], Sk2f(0, 0));
            } else if (PrimitiveType::kConics == fPrimitiveType &&
                       Verb::kMonotonicConicTo == verb) {
                fQuadPointInstances.push_back().setW(&geometry.points()[ptsIdx], Sk2f(0, 0),
                                                     geometry.getConicWeight(conicWeightIdx++));
            }
            ptsIdx += 2;
        }
    } else {
        fTriPointInstances.push_back().set(fPoints[0], fPoints[1], fPoints[3], Sk2f(0, 0));
        fPath.lineTo(fPoints[1]);
        fPath.lineTo(fPoints[3]);
        fPath.close();
    }
}

void CCPRGeometryView::DrawCoverageCountOp::onExecute(GrOpFlushState* state,
                                                      const SkRect& chainBounds) {
    GrResourceProvider* rp = state->resourceProvider();
    GrContext* context = state->gpu()->getContext();
    GrGLGpu* glGpu = GrBackendApi::kOpenGL == context->backend()
                             ? static_cast<GrGLGpu*>(state->gpu())
                             : nullptr;
    if (glGpu) {
        glGpu->handleDirtyContext();
        // GR_GL_CALL(glGpu->glInterface(), PolygonMode(GR_GL_FRONT_AND_BACK, GR_GL_LINE));
        GR_GL_CALL(glGpu->glInterface(), Enable(GR_GL_LINE_SMOOTH));
    }

    GrPipeline pipeline(GrScissorTest::kDisabled, SkBlendMode::kPlus,
                        state->drawOpArgs().fOutputSwizzle);

    std::unique_ptr<GrCCCoverageProcessor> proc;
    if (state->caps().shaderCaps()->geometryShaderSupport()) {
        proc = skstd::make_unique<GrGSCoverageProcessor>();
    } else {
        proc = skstd::make_unique<GrVSCoverageProcessor>();
    }

    if (!fView->fDoStroke) {
        proc->reset(fView->fPrimitiveType, rp);
        SkDEBUGCODE(proc->enableDebugBloat(kDebugBloat));

        SkSTArray<1, GrMesh> mesh;
        if (PrimitiveType::kCubics == fView->fPrimitiveType ||
            PrimitiveType::kConics == fView->fPrimitiveType) {
            sk_sp<GrGpuBuffer> instBuff(
                    rp->createBuffer(fView->fQuadPointInstances.count() * sizeof(QuadPointInstance),
                                     GrGpuBufferType::kVertex, kDynamic_GrAccessPattern,
                                     fView->fQuadPointInstances.begin()));
            if (!fView->fQuadPointInstances.empty() && instBuff) {
                proc->appendMesh(std::move(instBuff), fView->fQuadPointInstances.count(), 0, &mesh);
            }
        } else {
            sk_sp<GrGpuBuffer> instBuff(
                    rp->createBuffer(fView->fTriPointInstances.count() * sizeof(TriPointInstance),
                                     GrGpuBufferType::kVertex, kDynamic_GrAccessPattern,
                                     fView->fTriPointInstances.begin()));
            if (!fView->fTriPointInstances.empty() && instBuff) {
                proc->appendMesh(std::move(instBuff), fView->fTriPointInstances.count(), 0, &mesh);
            }
        }

        if (!mesh.empty()) {
            SkASSERT(1 == mesh.count());
            proc->draw(state, pipeline, nullptr, mesh.begin(), 1, this->bounds());
        }
    } else if (PrimitiveType::kConics != fView->fPrimitiveType) {  // No conic stroke support yet.
        GrCCStroker stroker(0,0,0);

        SkPaint p;
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(fView->fStrokeWidth);
        p.setStrokeJoin(SkPaint::kMiter_Join);
        p.setStrokeMiter(4);
        // p.setStrokeCap(SkPaint::kRound_Cap);
        stroker.parseDeviceSpaceStroke(fView->fPath, SkPathPriv::PointData(fView->fPath),
                                       SkStrokeRec(p), p.getStrokeWidth(), GrScissorTest::kDisabled,
                                       SkIRect::MakeWH(fView->width(), fView->height()), {0, 0});
        GrCCStroker::BatchID batchID = stroker.closeCurrentBatch();

        GrOnFlushResourceProvider onFlushRP(context->priv().drawingManager());
        stroker.prepareToDraw(&onFlushRP);

        SkIRect ibounds;
        this->bounds().roundOut(&ibounds);
        stroker.drawStrokes(state, proc.get(), batchID, ibounds);
    }

    if (glGpu) {
        context->resetContext(kMisc_GrGLBackendState);
    }
}

class CCPRGeometryView::Click : public Sample::Click {
public:
    Click(int ptIdx) : fPtIdx(ptIdx) {}

    void doClick(SkPoint points[]) {
        if (fPtIdx >= 0) {
            points[fPtIdx] += fCurr - fPrev;
        } else {
            for (int i = 0; i < 4; ++i) {
                points[i] += fCurr - fPrev;
            }
        }
    }

private:
    int fPtIdx;
};

Sample::Click* CCPRGeometryView::onFindClickHandler(SkScalar x, SkScalar y, ModifierKey) {
    for (int i = 0; i < 4; ++i) {
        if (PrimitiveType::kCubics != fPrimitiveType && 2 == i) {
            continue;
        }
        if (fabs(x - fPoints[i].x()) < 20 && fabsf(y - fPoints[i].y()) < 20) {
            return new Click(i);
        }
    }
    return new Click(-1);
}

bool CCPRGeometryView::onClick(Sample::Click* click) {
    Click* myClick = (Click*)click;
    myClick->doClick(fPoints);
    this->updateAndInval();
    return true;
}

bool CCPRGeometryView::onChar(SkUnichar unichar) {
        if (unichar >= '1' && unichar <= '4') {
            fPrimitiveType = PrimitiveType(unichar - '1');
            if (fPrimitiveType >= PrimitiveType::kWeightedTriangles) {
                fPrimitiveType = (PrimitiveType) ((int)fPrimitiveType + 1);
            }
            this->updateAndInval();
            return true;
        }
        float* valueToScale = nullptr;
        if (fDoStroke) {
            valueToScale = &fStrokeWidth;
        } else if (PrimitiveType::kConics == fPrimitiveType) {
            valueToScale = &fConicWeight;
        }
        if (valueToScale) {
            if (unichar == '+') {
                *valueToScale *= 2;
                this->updateAndInval();
                return true;
            }
            if (unichar == '+' || unichar == '=') {
                *valueToScale *= 5/4.f;
                this->updateAndInval();
                return true;
            }
            if (unichar == '-') {
                *valueToScale *= 4/5.f;
                this->updateAndInval();
                return true;
            }
            if (unichar == '_') {
                *valueToScale *= .5f;
                this->updateAndInval();
                return true;
            }
        }
        if (unichar == 'D') {
            SkDebugf("    SkPoint fPoints[4] = {\n");
            SkDebugf("        {%ff, %ff},\n", fPoints[0].x(), fPoints[0].y());
            SkDebugf("        {%ff, %ff},\n", fPoints[1].x(), fPoints[1].y());
            SkDebugf("        {%ff, %ff},\n", fPoints[2].x(), fPoints[2].y());
            SkDebugf("        {%ff, %ff}\n", fPoints[3].x(), fPoints[3].y());
            SkDebugf("    };\n");
            return true;
        }
        if (unichar == 'S') {
            fDoStroke = !fDoStroke;
            this->updateAndInval();
        }
        return false;
}

DEF_SAMPLE(return new CCPRGeometryView;)

#endif  // SK_SUPPORT_GPU
