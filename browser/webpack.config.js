const path = require('path');
const webpack = require('webpack')

module.exports = {
    entry: {
        TVTDataBroadcastingWV2: './src/TVTDataBroadcastingWV2.ts',
        // HTMLアプリのページへ注入するAPI(C++が読み込んで AddScriptToExecuteOnDocumentCreated に渡す)
        hybridcast_api: {
            import: './src/hybridcast/hybridcast_api.ts',
            library: { name: 'HybridcastApi', type: 'var' },
        },
    },
    output: {
        path: path.resolve(__dirname, 'dist'),
        filename: '[name].js',
    },
    module: {
        rules: [
            {
                test: /\.ts$/,
                use: 'ts-loader',
            },
        ],
    },
    resolve: {
        extensions: [
            '.ts', '.js',
        ],
    },
};
