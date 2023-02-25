#include "StratusFrameBuffer.h"
#include "GL/gl3w.h"
#include <iostream>
#include "StratusLog.h"
#include "StratusApplicationThread.h"

namespace stratus {
    class FrameBufferImpl {
        GLuint _fbo;
        std::vector<Texture> _colorAttachments;
        std::vector<GLenum> _glColorAttachments; // For use with glDrawBuffers
        Texture _depthStencilAttachment;
        mutable GLenum _currentBindingPoint = 0;
        bool _valid = false;

    public:
        FrameBufferImpl() {
            glGenFramebuffers(1, &_fbo);
        }

        ~FrameBufferImpl() {
            if (ApplicationThread::Instance()->CurrentIsApplicationThread()) {
                glDeleteFramebuffers(1, &_fbo);
            }
            else {
                auto buffer = _fbo;
                ApplicationThread::Instance()->Queue([buffer]() { auto fbo = buffer; glDeleteFramebuffers(1, &fbo); });
            }
        }

        // No copying
        FrameBufferImpl(const FrameBufferImpl &) = delete;
        FrameBufferImpl(FrameBufferImpl &&) = delete;
        FrameBufferImpl & operator=(const FrameBufferImpl &) = delete;
        FrameBufferImpl & operator=(FrameBufferImpl &&) = delete;

        void clear(const glm::vec4 & rgba) {
            bool bindAndUnbind = true;
            if (_currentBindingPoint != 0) bindAndUnbind = false;
            if (bindAndUnbind) bind();
            glDepthMask(GL_TRUE);
            glStencilMask(GL_TRUE);
            glDrawBuffers(_glColorAttachments.size(), _glColorAttachments.data());
            glClearColor(rgba.r, rgba.g, rgba.b, rgba.a);
            glClearDepthf(1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            if (bindAndUnbind) unbind();
        }

        void ClearColorLayer(const glm::vec4& rgba, const size_t colorIndex, const int layer) {
            if (_colorAttachments.size() < colorIndex) {
                throw std::runtime_error("Color index exceeds maximum total bound color buffers");
            }
            
            // TODO: There is a much better way to do this
            // See https://registry.khronos.org/OpenGL-Refpages/gl4/html/glFramebufferTextureLayer.xhtml
            // Followed by a regular glClear of the color attachment
            Texture& color = _colorAttachments[colorIndex];
            color.clearLayer(0, layer, (const void *)&rgba[0]);
        }

        void ClearDepthStencilLayer(const int layer) {
            if (_depthStencilAttachment == Texture()) {
                throw std::runtime_error("Attempt to clear null depth/stencil attachment");
            }

            float val = 1.0f;
            // TODO: There is a much better way to do this
            // See https://registry.khronos.org/OpenGL-Refpages/gl4/html/glFramebufferTextureLayer.xhtml
            // Followed by a regular glClear of the depth stencil attachment
            //std::vector<float> data(_depthStencilAttachment.width() * _depthStencilAttachment.height(), val);
            _depthStencilAttachment.clearLayer(0, layer, (const void *)&val);
        }

        void bind() const {
            _bind(GL_FRAMEBUFFER);
        }

        void unbind() const {
            if (_currentBindingPoint == -1) return;
            glBindFramebuffer(_currentBindingPoint, 0);
            _currentBindingPoint = 0;
        }

        void SetColorTextureLayer(const int attachmentNum, const int mipLevel, const int layer) {
            if (_colorAttachments.size() < attachmentNum) {
                throw std::runtime_error("Attachment number exceeds amount of attached color textures");
            }

            glNamedFramebufferTextureLayer(
                _fbo, 
                GL_COLOR_ATTACHMENT0 + attachmentNum,
                *(GLuint *)getColorAttachments()[attachmentNum].underlying(),
                mipLevel, layer
            );
        }

        void SetDepthTextureLayer(const int layer) {
            if (_depthStencilAttachment == Texture()) {
                throw std::runtime_error("Attempt to use null depth/stencil attachment");
            }

            glNamedFramebufferTextureLayer(
                _fbo, 
                GL_DEPTH_ATTACHMENT,
                *(GLuint *)getDepthStencilAttachment()->underlying(),
                0, layer
            );
        }

        void setAttachments(const std::vector<Texture> & attachments) {
            if (_colorAttachments.size() > 0 || _depthStencilAttachment.valid()) throw std::runtime_error("setAttachments called twice");
            _valid = true;

            bind();

            // We can only have 1 max for each
            int numDepthStencilAttachments = 0;

            // In the case of multiple color attachments we need to let OpenGL know
            std::vector<uint32_t> drawBuffers;

            for (Texture tex : attachments) {
                tex.bind();
                GLuint underlying = *(GLuint *)tex.underlying();
                if (tex.format() == TextureComponentFormat::DEPTH) {
                    if (numDepthStencilAttachments > 0) throw std::runtime_error("More than one depth attachment present");
                    /*
                    glFramebufferTexture2D(GL_FRAMEBUFFER,
                        GL_DEPTH_ATTACHMENT,
                        GL_TEXTURE_2D,
                        underlying,
                        0);
                    */
                    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, underlying, 0);
                    ++numDepthStencilAttachments;
                    _depthStencilAttachment = tex;
                }
                else if (tex.format() == TextureComponentFormat::DEPTH_STENCIL) {
                    if (numDepthStencilAttachments > 0) throw std::runtime_error("More than one depth_stencil attachment present");
                    /*
                    glFramebufferTexture2D(GL_FRAMEBUFFER,
                        GL_DEPTH_STENCIL_ATTACHMENT,
                        GL_TEXTURE_2D,
                        underlying,
                        0);
                    */
                    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, underlying, 0);
                    ++numDepthStencilAttachments;
                    _depthStencilAttachment = tex;
                }
                else {
                    GLenum color = GL_COLOR_ATTACHMENT0 + drawBuffers.size();
                    _glColorAttachments.push_back(color);
                    drawBuffers.push_back(color);
                    /*
                    glFramebufferTexture2D(GL_FRAMEBUFFER, 
                        color, 
                        GL_TEXTURE_2D, 
                        underlying, 
                        0);
                    */
                    glFramebufferTexture(GL_FRAMEBUFFER, color, underlying, 0);
                    _colorAttachments.push_back(tex);
                }
                tex.unbind();
            }

            if (drawBuffers.size() == 0) {
                // Tell OpenGL we won't be using a color buffer
                glDrawBuffer(GL_NONE);
                glReadBuffer(GL_NONE);
            }
            else {
                glDrawBuffers(drawBuffers.size(), &drawBuffers[0]);
            }

            // Validity check
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                std::cerr << "[error] Generating frame buffer with attachments failed" << std::endl;
                _valid = false;
            }

            unbind();
        }

        bool valid() const {
            return _valid;
        }

        void copyFrom(const FrameBufferImpl & other, const BufferBounds & from, const BufferBounds & to, BufferBit bit, BufferFilter filter) {
            // Blit to default framebuffer - not that the framebuffer you are writing to has to match the internal format
            // of the framebuffer you are reading to!
            GLbitfield mask = 0;
            if (bit & COLOR_BIT) mask |= GL_COLOR_BUFFER_BIT;
            if (bit & DEPTH_BIT) mask |= GL_DEPTH_BUFFER_BIT;
            if (bit & STENCIL_BIT) mask |= GL_STENCIL_BUFFER_BIT;
            GLenum blitFilter = (filter == BufferFilter::NEAREST) ? GL_NEAREST : GL_LINEAR;
            glBlitNamedFramebuffer(other._fbo, _fbo, from.startX, from.startY, from.endX, from.endY, to.startX, to.startY, to.endX, to.endY, mask, blitFilter);
        }

        const std::vector<Texture> & getColorAttachments() const {
            return _colorAttachments;
        }

        const Texture * getDepthStencilAttachment() const {
            return &_depthStencilAttachment;
        }

        void * underlying() const {
            return (void *)&_fbo;
        }

    private:
        void _bind(GLenum bindingPoint) const {
            glBindFramebuffer(bindingPoint, _fbo);
            _currentBindingPoint = bindingPoint;
        }
    };

    FrameBuffer::FrameBuffer() {}
    FrameBuffer::FrameBuffer(const std::vector<Texture> & attachments) {
        _fbo = std::make_shared<FrameBufferImpl>();
        _fbo->setAttachments(attachments);
    }
    FrameBuffer::~FrameBuffer() {}

    // Clears the color, depth and stencil buffers using rgba
    void FrameBuffer::clear(const glm::vec4 & rgba) {
        _fbo->clear(rgba);
    }

    void FrameBuffer::ClearColorLayer(const glm::vec4& rgba, const size_t colorIndex, const int layer) {
        _fbo->ClearColorLayer(rgba, colorIndex, layer);
    }

    void FrameBuffer::ClearDepthStencilLayer(const int layer) {
        _fbo->ClearDepthStencilLayer(layer);
    }

    // from = rectangular region in *other* to copy from
    // to = rectangular region in *this* to copy to
    void FrameBuffer::copyFrom(const FrameBuffer & other, const BufferBounds & from, const BufferBounds & to, BufferBit bit, BufferFilter filter) {
        _fbo->copyFrom(*other._fbo, from, to, bit, filter);
    }

    const std::vector<Texture> & FrameBuffer::getColorAttachments() const { return _fbo->getColorAttachments(); }
    const Texture * FrameBuffer::getDepthStencilAttachment() const        { return _fbo->getDepthStencilAttachment(); }

    void FrameBuffer::bind() const         { _fbo->bind(); }
    void FrameBuffer::unbind() const       { _fbo->unbind(); }
    bool FrameBuffer::valid() const        { return _fbo != nullptr && _fbo->valid(); }
    void * FrameBuffer::underlying() const { return _fbo->underlying(); }

    void FrameBuffer::SetColorTextureLayer(const int attachmentNum, const int mipLevel, const int layer) { 
        _fbo->SetColorTextureLayer(attachmentNum, mipLevel, layer); 
    }

    void FrameBuffer::SetDepthTextureLayer(const int layer) { 
        _fbo->SetDepthTextureLayer(layer);
    }
}