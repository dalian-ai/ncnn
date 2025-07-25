# Copyright 2021 Tencent
# SPDX-License-Identifier: BSD-3-Clause

import torch
import torch.nn as nn
import torch.nn.functional as F
from packaging import version

class Model(nn.Module):
    def __init__(self):
        super(Model, self).__init__()

        self.conv_0 = nn.Conv2d(in_channels=12, out_channels=16, kernel_size=3)
        self.bn_0 = nn.BatchNorm2d(num_features=16)
        self.conv_1 = nn.Conv2d(in_channels=16, out_channels=20, kernel_size=(2,4), stride=(2,1), padding=2, dilation=1)
        self.bn_1 = nn.BatchNorm2d(num_features=20)
        self.conv_2 = nn.Conv2d(in_channels=20, out_channels=24, kernel_size=(1,3), stride=1, padding=(2,4), dilation=1, groups=1, bias=False)
        self.bn_2 = nn.BatchNorm2d(num_features=24)
        if version.parse(torch.__version__) < version.parse('1.9'):
            self.conv_3 = nn.Conv2d(in_channels=24, out_channels=28, kernel_size=(5,4), stride=1, padding=0, dilation=1, groups=4, bias=True)
        else:
            self.conv_3 = nn.Conv2d(in_channels=24, out_channels=28, kernel_size=(5,4), stride=1, padding='valid', dilation=1, groups=4, bias=True)
        self.bn_3 = nn.BatchNorm2d(num_features=28)
        if version.parse(torch.__version__) < version.parse('1.9'):
            self.conv_4 = nn.Conv2d(in_channels=28, out_channels=32, kernel_size=3, stride=1, padding=1, dilation=(1,2), groups=2, bias=False, padding_mode='zeros')
        else:
            self.conv_4 = nn.Conv2d(in_channels=28, out_channels=32, kernel_size=3, stride=1, padding='same', dilation=(1,2), groups=2, bias=False, padding_mode='zeros')
        self.bn_4 = nn.BatchNorm2d(num_features=32)
        self.conv_5 = nn.Conv2d(in_channels=32, out_channels=32, kernel_size=2, stride=2, padding=3, dilation=1, groups=32, bias=True, padding_mode='reflect')
        self.bn_5 = nn.BatchNorm2d(num_features=32)
        self.conv_6 = nn.Conv2d(in_channels=32, out_channels=28, kernel_size=2, stride=1, padding=2, dilation=1, groups=1, bias=False, padding_mode='replicate')
        self.bn_6 = nn.BatchNorm2d(num_features=28)
        #self.conv_7 = nn.Conv2d(in_channels=28, out_channels=24, kernel_size=3, stride=2, padding=(5,6), dilation=2, groups=1, bias=True, padding_mode='circular')
        #self.bn_7 = nn.BatchNorm2d(num_features=24)

    def forward(self, x):
        x = self.conv_0(x)
        x = self.bn_0(x)
        x = self.conv_1(x)
        x = self.bn_1(x)
        x = self.conv_2(x)
        x = self.bn_2(x)
        x = self.conv_3(x)
        x = self.bn_3(x)
        x = self.conv_4(x)
        x = self.bn_4(x)
        x = self.conv_5(x)
        x = self.bn_5(x)
        x = self.conv_6(x)
        x = self.bn_6(x)
        #x = self.conv_7(x)
        #x = self.bn_7(x)

        return x

def test():
    net = Model()
    net.eval()

    torch.manual_seed(0)
    x = torch.rand(1, 12, 64, 64)

    a = net(x)

    # export torchscript
    mod = torch.jit.trace(net, x)
    mod.save("test_pnnx_fuse_conv2d_batchnorm2d.pt")

    # torchscript to pnnx
    import os
    os.system("../src/pnnx test_pnnx_fuse_conv2d_batchnorm2d.pt inputshape=[1,12,64,64]")

    # pnnx inference
    import test_pnnx_fuse_conv2d_batchnorm2d_pnnx
    b = test_pnnx_fuse_conv2d_batchnorm2d_pnnx.test_inference()

    return torch.allclose(a, b, 1e-4, 1e-4)

if __name__ == "__main__":
    if test():
        exit(0)
    else:
        exit(1)
