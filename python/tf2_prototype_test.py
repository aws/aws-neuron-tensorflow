# Copyright Amazon Web Services and its Affiliates. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
import unittest
import numpy as np
import tensorflow as tf #supposed to be tf2
import random
from itertools import product
import shutil

#each number represents the number of random 
#parameters from that catergory 
#that will be used in testing

NUM_ACTIVATIONS = 1  #Max is 6
NUM_INPUT_UNITS = 1  #Max is 1024 (would not use more than 10)
NUM_OUTPUT_UNITS = 1 #Max is 1024 (would not use more than 10)
NUM_MAGIC_NUMBERS = 3#Max is 10
NUM_KERNEL_SIZES = 1 #Max is 2
NUM_POWERS = 2       #Max is 11


#here are the parameter lists
inputNumUnits = list(range(1, 1025))
outputNumUnits = list(range(1, 1025))
magicNumbers = [28, 14, 7, 224, 112, 56, 28, 299, 150, 75]
kernelSizes = [1, 3]
activations = ['softmax',  'relu', 'tanh', 'sigmoid',  'exponential', 'linear']
powersOfTwo = [2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048]


#randomize the order
random.shuffle(inputNumUnits)
random.shuffle(outputNumUnits)
random.shuffle(magicNumbers)
random.shuffle(kernelSizes)
random.shuffle(activations)
random.shuffle(powersOfTwo)

#Pick the first n params based on specified constants above 
inputNumUnits = inputNumUnits[0:NUM_INPUT_UNITS]
outputNumUnits = outputNumUnits[0:NUM_OUTPUT_UNITS]
magicNumbers = magicNumbers[0:NUM_MAGIC_NUMBERS]
kernelSizes = kernelSizes[0:NUM_KERNEL_SIZES]
activations = activations[0:NUM_ACTIVATIONS]
powersOfTwo = powersOfTwo[0:NUM_POWERS]


class TestKerasTF2(unittest.TestCase):
    #This function tests a basic NN with two dense layers.
    #It has 3 paramaters which vary.
    #1. Number of input units
    #2. Number of output units
    #3. The type of activation function that the input layer uses
    #@parameterized.expand()
    def test_flatten_dense_dropout(self):
        
        param_list = list(product(inputNumUnits, activations, outputNumUnits))
        for inu, a, onu in param_list:
            #subTest allows us to generate tests dynamically
            #if one of the subTests fail, the error message
            #along with the inputs (inu a onu) will be displayed.
            #however this will still show up as 1 test even though
            #there can be many subTests

            with self.subTest(inputNumUnits = inu, activations = a, outputNumUnits = onu):
                model = tf.keras.models.Sequential([
                #tf.keras.layers is tf2 syntax
                tf.keras.layers.Flatten(input_shape=(28,28,1)),
                tf.keras.layers.Dense(inu, activation=a),
                tf.keras.layers.Dropout(0.2),
                tf.keras.layers.Dense(onu)])

                # Export SavedModel
                model_dir = './keras_flatten_dense_dropout'
                shutil.rmtree(model_dir, ignore_errors=True)
                tf.keras.models.save_model(model, model_dir)

                #we would then complie using TF Neuron with 2.0
                #support but this is just a prototype so we 
                #skip that step for now


                reloaded_model = tf.keras.models.load_model(model_dir)

                #in real test this would actually be a compiled model
                compiled_model = tf.keras.models.load_model(model_dir)

                test_input = np.random.random((1, 28, 28))
                #actual test would test compiler model on inf1
                #versus tf2 model on cpu
                np.testing.assert_allclose(
                    reloaded_model(test_input, training=False),
                    compiled_model(test_input, training=False))


    def test_conv2d_conv2d_flatten_dense(self):
        
        param_list = list(product(inputNumUnits, activations, outputNumUnits, kernelSizes))
        for inu, a, onu, ks in param_list:
            #subTest allows us to generate tests dynamically
            #if one of the subTests fail, the error message
            #along with the inputs (inu a onu) will be displayed.
            #however this will still show up as 1 test even though
            #there can be many subTests

            with self.subTest(inputNumUnits=inu, activations=a, outputNumUnits=onu, kernelSizes=ks):
                model = tf.keras.models.Sequential([
                #tf.keras.layers is tf2 syntax
                tf.keras.layers.Conv2D(inu, kernel_size=ks, 
                                        activation=a, input_shape=(28,28,1)),
                tf.keras.layers.Conv2D(inu, kernel_size=ks, activation=a),
                tf.keras.layers.Flatten(),
                tf.keras.layers.Dense(onu)])

                # Export SavedModel
                model_dir = './keras_conv2d_conv2d_flatten_dense'
                shutil.rmtree(model_dir, ignore_errors=True)
                tf.keras.models.save_model(model, model_dir)

                #we would then complie using TF Neuron with 2.0
                #support but this is just a prototype so we 
                #skip that step for now


                reloaded_model = tf.keras.models.load_model(model_dir)

                #in real test this would actually be a compiled model
                compiled_model = tf.keras.models.load_model(model_dir)

                test_input = np.random.random((1, 28, 28, 1))


                #actual test would test compiler model on inf1
                #versus tf2 model on cpu
                np.testing.assert_allclose(
                    reloaded_model(test_input, training=False),
                    compiled_model(test_input, training=False))

    def test_lstm_lstm_dense_dense(self):
        
        param_list = list(product(inputNumUnits, activations, outputNumUnits))
        for inu, a, onu in param_list:
            with self.subTest(inputNumUnits=inu, activations=a, outputNumUnits=onu):
                model = tf.keras.models.Sequential([
                #tf.keras.layers is tf2 syntax
                tf.keras.layers.LSTM(inu, activation=a, input_shape=(28,28), return_sequences=True),
                tf.keras.layers.LSTM(inu, activation=a),
                tf.keras.layers.Dense(onu, activation=a),
                tf.keras.layers.Dense(10, activation=a)])

                # Export SavedModel
                model_dir = './keras_lstm_lstm_dense_dense'
                shutil.rmtree(model_dir, ignore_errors=True)
                tf.keras.models.save_model(model, model_dir)

                #we would then complie using TF Neuron with 2.0
                #support but this is just a prototype so we 
                #skip that step for now


                reloaded_model = tf.keras.models.load_model(model_dir)

                #in real test this would actually be a compiled model
                compiled_model = tf.keras.models.load_model(model_dir)

                test_input = np.random.random((1, 28, 28))


                #actual test would test compiler model on inf1
                #versus tf2 model on cpu
                np.testing.assert_allclose(
                    reloaded_model(test_input, training=False),
                    compiled_model(test_input, training=False))







    def test_maxpool2d(self):
        #A simple test that is only parameterized by inputNumUnits
        #which in this case describes the size of the square input
        
        param_list = list(inputNumUnits)
        for inu in param_list:
            #subTest allows us to generate tests dynamically
            #if one of the subTests fail, the error message
            #along with the inputs (inu a onu) will be displayed.
            #however this will still show up as 1 test even though
            #there can be many subTests

            with self.subTest(inputNumUnits=inu):
                model = tf.keras.models.Sequential([
                #tf.keras.layers is tf2 syntax
                tf.keras.layers.MaxPool2D(pool_size=(2,2), strides=1, padding='same', input_shape=(inu, inu, 1))])

                # Export SavedModel
                model_dir = './keras_maxpool2d'
                shutil.rmtree(model_dir, ignore_errors=True)
                tf.keras.models.save_model(model, model_dir)

                #we would then complie using TF Neuron with 2.0
                #support but this is just a prototype so we 
                #skip that step for now


                reloaded_model = tf.keras.models.load_model(model_dir)

                #in real test this would actually be a compiled model
                compiled_model = tf.keras.models.load_model(model_dir)

                test_input = np.random.random((1, inu, inu, 1))


                #actual test would test compiler model on inf1
                #versus tf2 model on cpu
                np.testing.assert_allclose(
                    reloaded_model(test_input, training=False),
                    compiled_model(test_input, training=False))


    def test_toy_resnet(self):
        inputs = tf.keras.Input(shape=(32, 32, 3), name="img")
        x = tf.keras.layers.Conv2D(32, 3, activation="relu")(inputs)
        x = tf.keras.layers.Conv2D(64, 3, activation="relu")(x)
        block_1_output = tf.keras.layers.MaxPooling2D(3)(x)

        x = tf.keras.layers.Conv2D(64, 3, activation="relu", padding="same")(block_1_output)
        x = tf.keras.layers.Conv2D(64, 3, activation="relu", padding="same")(x)
        block_2_output = tf.keras.layers.add([x, block_1_output])

        x = tf.keras.layers.Conv2D(64, 3, activation="relu", padding="same")(block_2_output)
        x = tf.keras.layers.Conv2D(64, 3, activation="relu", padding="same")(x)
        block_3_output = tf.keras.layers.add([x, block_2_output])

        x = tf.keras.layers.Conv2D(64, 3, activation="relu")(block_3_output)
        x = tf.keras.layers.GlobalAveragePooling2D()(x)
        x = tf.keras.layers.Dense(256, activation="relu")(x)
        x = tf.keras.layers.Dropout(0.5)(x)
        outputs = tf.keras.layers.Dense(10)(x)

        model = tf.keras.Model(inputs, outputs, name="toy_resnet")
        model.summary() 
        model_dir = './keras_toy_resnet'
        shutil.rmtree(model_dir, ignore_errors=True)
        tf.keras.models.save_model(model, model_dir)

        #we would then complie using TF Neuron with 2.0
        #support but this is just a prototype so we 
        #skip that step for now


        reloaded_model = tf.keras.models.load_model(model_dir)

        #in real test this would actually be a compiled model
        compiled_model = tf.keras.models.load_model(model_dir)

        test_input = np.random.random((1, 32, 32, 3))


        #actual test would test compiler model on inf1
        #versus tf2 model on cpu
        np.testing.assert_allclose(
            reloaded_model(test_input, training=False),
            compiled_model(test_input, training=False))

    def test_multiple_io(self):
        num_tags = 12  # Number of unique issue tags
        num_words = 10000  # Size of vocabulary obtained when preprocessing text data
        num_departments = 4  # Number of departments for predictions

        title_input = tf.keras.Input(
            shape=(None,), name="title"
        )  # Variable-length sequence of ints
        body_input = tf.keras.Input(shape=(None,), name="body")  # Variable-length sequence of ints
        tags_input = tf.keras.Input(
            shape=(num_tags,), name="tags"
        )  # Binary vectors of size `num_tags`

        # Embed each word in the title into a 64-dimensional vector
        title_features = tf.keras.layers.Embedding(num_words, 64)(title_input)
        # Embed each word in the text into a 64-dimensional vector
        body_features = tf.keras.layers.Embedding(num_words, 64)(body_input)

        # Reduce sequence of embedded words in the title into a single 128-dimensional vector
        title_features = tf.keras.layers.LSTM(128)(title_features)
        # Reduce sequence of embedded words in the body into a single 32-dimensional vector
        body_features = tf.keras.layers.LSTM(32)(body_features)

        # Merge all available features into a single large vector via concatenation
        x = tf.keras.layers.concatenate([title_features, body_features, tags_input])

        # Stick a logistic regression for priority prediction on top of the features
        priority_pred = tf.keras.layers.Dense(1, name="priority")(x)
        # Stick a department classifier on top of the features
        department_pred = tf.keras.layers.Dense(num_departments, name="department")(x)

        # Instantiate an end-to-end model predicting both priority and department
        model = tf.keras.Model(
            inputs=[title_input, body_input, tags_input],
            outputs=[priority_pred, department_pred],
        )

        model_dir = './keras_multiple_io'
        shutil.rmtree(model_dir, ignore_errors=True)
        tf.keras.models.save_model(model, model_dir)

        #we would then complie using TF Neuron with 2.0
        #support but this is just a prototype so we 
        #skip that step for now


        reloaded_model = tf.keras.models.load_model(model_dir)

        #in real test this would actually be a compiled model
        compiled_model = tf.keras.models.load_model(model_dir)

        # Dummy input data
        title_data = np.random.randint(num_words, size=(1280, 10))
        body_data = np.random.randint(num_words, size=(1280, 100))
        tags_data = np.random.randint(2, size=(1280, num_tags)).astype("float32")

        result_ref = reloaded_model([title_data, body_data, tags_data], training=False)
        result_neuron = reloaded_model([title_data, body_data, tags_data], training=False)

        #actual test would test compiler model on inf1
        #versus tf2 model on cpu
        np.testing.assert_allclose(result_ref[0], result_neuron[0])
        np.testing.assert_allclose(result_ref[1], result_neuron[1])


