import Foundation
import Darwin
import Metal
let device = MTLCreateSystemDefaultDevice()!
if let path = ProcessInfo.processInfo.environment["REIMS_SYNC_DYLIB"] { guard dlopen(path,RTLD_NOW|RTLD_LOCAL) != nil else { fatalError(String(cString:dlerror())) } }
let queue = device.makeCommandQueue()!
var failed = false
for index in 0..<2 {
 let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm_srgb, width: 356, height: 183, mipmapped: false)
 d.storageMode = .private; d.usage = [.renderTarget,.shaderRead]; d.textureType = .type2DMultisample; d.sampleCount = 4
 let msaa = device.makeTexture(descriptor:d)!
 d.textureType = .type2D; d.sampleCount = 1; d.storageMode = .managed
 let resolved = device.makeTexture(descriptor:d)!
 let cb = queue.makeCommandBuffer()!
 let pass = MTLRenderPassDescriptor(); let color = pass.colorAttachments[0]!
 color.texture = CommandLine.arguments.contains("single") ? resolved : msaa; color.loadAction = .clear; color.storeAction = .store
 color.clearColor = MTLClearColorMake(0.947295,0.913085,0.84686,1)
 if !CommandLine.arguments.contains("same-pass") {
 cb.makeRenderCommandEncoder(descriptor:pass)!.endEncoding()
 color.loadAction = .load
 }
 color.storeAction = CommandLine.arguments.contains("single") ? .store : .multisampleResolve; color.resolveTexture = CommandLine.arguments.contains("single") ? nil : resolved
 cb.makeRenderCommandEncoder(descriptor:pass)!.endEncoding()
 let blit = cb.makeBlitCommandEncoder()!; blit.synchronize(texture:resolved,slice:0,level:0); blit.endEncoding()
 cb.commit(); cb.waitUntilCompleted()
 var bytes = [UInt8](repeating:0,count:356*183*4)
 resolved.getBytes(&bytes,bytesPerRow:356*4,from:MTLRegionMake2D(0,0,356,183),mipmapLevel:0)
 print("pass=\(index) status=\(cb.status.rawValue) first=\(bytes.prefix(4)) nonzero=\(bytes.filter{$0 != 0}.count)")
 if cb.status != .completed || bytes[0] == 0 { failed = true }
}

exit(failed ? 1 : 0)
